// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// /dev/fsa: the IOSU file system service. Requests arrive as FSAShimBuffer
// blocks prepared by coreinit's FS layer and are carried out on the virtual
// file system (fsc). Results are FSA_RESULT codes, or an element count for
// read, write and append.
#include "fsa_types.h"
#include "iosu_fsa.h"
#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "Cafe/Filesystem/fsc.h"
#include "util/helpers/helpers.h"
#include "Cafe/OS/libs/coreinit/coreinit_FS.h" // FSA_CMD_FLAG_SET_POS
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/MMU/MMU.h"

namespace iosu
{
	namespace fsa
	{
		using namespace iosu::kernel;

		namespace
		{
			/* ---- per client state ---- */

			struct Client
			{
				bool open = false;
				std::string cwd;
			};

			std::array<Client, 624> g_clients;

			/* ---- open file and directory handles ---- */

			// Handle value: slot index in the upper 16 bits, a 16 bit serial below.
			class NodeTable
			{
			public:
				bool Insert(FSCVirtualFile* node, FSResHandle& handleOut)
				{
					for (size_t slot = 0; slot < m_slots.size(); slot++)
					{
						Slot& s = m_slots[slot];
						if (s.node)
							continue;
						s.serial = (uint16)m_serialCounter++;
						s.node = node;
						handleOut = (FSResHandle)(((uint32)slot << 16) | s.serial);
						return true;
					}
					cemuLog_log(LogType::Force, "FSA: handle table is full");
					return false;
				}

				FSCVirtualFile* Find(FSResHandle handle) const
				{
					const Slot* s = Resolve(handle);
					return s ? s->node : nullptr;
				}

				void Erase(FSResHandle handle)
				{
					if (Slot* s = const_cast<Slot*>(Resolve(handle)))
						s->node = nullptr;
				}

			private:
				struct Slot
				{
					FSCVirtualFile* node = nullptr; // null while unused
					uint16 serial = 0;
				};

				const Slot* Resolve(FSResHandle handle) const
				{
					const uint32 slot = ((uint32)handle >> 16) & 0xFFFF;
					if (slot >= m_slots.size())
						return nullptr;
					const Slot& s = m_slots[slot];
					if (!s.node || s.serial != (uint16)((uint32)handle & 0xFFFF))
						return nullptr;
					return &s;
				}

				uint32 m_serialCounter = 1;
				std::array<Slot, 960> m_slots;
			};

			NodeTable g_openFiles;
			NodeTable g_openDirs;

			FSA_RESULT FromFscStatus(sint32 status)
			{
				switch (status)
				{
				case FSC_STATUS_OK:
					return FSA_RESULT::OK;
				case FSC_STATUS_FILE_NOT_FOUND:
					return FSA_RESULT::NOT_FOUND;
				case FSC_STATUS_ALREADY_EXISTS:
					return FSA_RESULT::ALREADY_EXISTS;
				default:
					cemu_assert_unimplemented();
					return FSA_RESULT::FATAL_ERROR;
				}
			}

			/* ---- path handling ---- */

			// cut 'path' back to the last slash it contains (that slash is removed too)
			void StripLastSegment(std::string& path)
			{
				const size_t slash = path.rfind('/');
				path.resize(slash == std::string::npos ? 0 : slash);
			}

			// Builds the absolute fsc path for a client supplied path. Relative
			// paths start at the client's working directory. "." and ".." segments
			// are resolved textually, repeated slashes collapse.
			std::string ResolvePath(const Client& client, std::string_view in, bool withTrailingSlash = false)
			{
				std::string out;
				if (in.empty())
				{
					out = client.cwd;
					if (withTrailingSlash)
						out += '/';
					return out;
				}
				cemu_assert_debug(in[0] != '\\');
				if (in[0] == '/')
					out = "/";
				else if (in[0] == '~')
				{
					cemu_assert_debug(false); // not seen in practice
					in.remove_prefix(1);
					out = "/";
				}
				else
					out = client.cwd;

				size_t pos = 0;
				while (pos < in.size())
				{
					if (in[pos] == '/')
					{
						pos++;
						continue;
					}
					const std::string_view rest = in.substr(pos);
					if (rest == ".." || rest.starts_with("../"))
					{
						StripLastSegment(out);
						pos += (rest.size() == 2) ? 2 : 3;
						continue;
					}
					if (rest.starts_with("./"))
					{
						pos += 2;
						continue;
					}
					size_t end = in.find('/', pos);
					if (end == std::string_view::npos)
						end = in.size();
					if (!out.empty() && out.back() != '/')
						out += '/';
					out.append(in.substr(pos, end - pos));
					pos = end;
				}
				if (withTrailingSlash && (out.empty() || out.back() != '/'))
					out += '/';
				return out;
			}

			FSCVirtualFile* OpenNode(const Client& client, std::string_view path, FSC_ACCESS_FLAG flags, sint32& status)
			{
				const std::string full = ResolvePath(client, path);
				return fsc_open(full.c_str(), flags, &status);
			}

			/* ---- stat helpers ---- */

			void DescribeNode(FSCVirtualFile* node, FSStat_t* stat)
			{
				memset(stat, 0, sizeof(FSStat_t));
				FSFlag kind = FSFlag::NONE;
				if (fsc_isDirectory(node))
				{
					// directories report size 0 (titles such as Hyrule Warriors depend on it)
					kind |= FSFlag::IS_DIR;
					stat->size = 0;
				}
				else if (fsc_isFile(node))
				{
					stat->size = fsc_getFileSize(node);
					kind |= FSFlag::IS_FILE;
				}
				else
					cemu_assert_suspicious();
				stat->permissions = 0x777;
				stat->flag = kind;
			}

			FSA_RESULT StatPath(const Client& client, const char* path, FSStat_t* stat)
			{
				sint32 status;
				FSCVirtualFile* node = OpenNode(client, path, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::OPEN_DIR, status);
				if (!node)
					return FromFscStatus(status);
				DescribeNode(node, stat);
				delete node;
				return FSA_RESULT::OK;
			}

			/* ---- request handlers ---- */

			// translate an fopen style mode string into fsc access flags
			FSC_ACCESS_FLAG AccessForMode(std::string_view mode, bool& append)
			{
				using F = FSC_ACCESS_FLAG;
				const F readWrite = F::READ_PERMISSION | F::WRITE_PERMISSION;
				append = false;
				if (mode == "r" || mode == "rb")
					return F::READ_PERMISSION;
				if (mode == "r+")
					return readWrite;
				if (mode == "w" || mode == "w+" || mode == "wb")
					return readWrite | F::FILE_ALWAYS_CREATE;
				if (mode == "a+")
				{
					append = true;
					return readWrite | F::FILE_ALLOW_CREATE | F::IS_APPEND;
				}
				if (mode == "a")
				{
					append = true;
					return F::WRITE_PERMISSION | F::FILE_ALLOW_CREATE | F::IS_APPEND;
				}
				cemu_assert_debug(false); // unknown mode string
				return F::NONE;
			}

			FSA_RESULT OpenFile(Client& client, FSAShimBuffer* shim)
			{
				const char* path = (const char*)shim->request.cmdOpenFile.path;
				const char* mode = (const char*)shim->request.cmdOpenFile.mode;
				shim->response.cmdOpenFile.fileHandleOutput = (uint32)FS_INVALID_HANDLE_VALUE;
				bool append;
				const FSC_ACCESS_FLAG access = AccessForMode(mode, append) | FSC_ACCESS_FLAG::OPEN_DIR | FSC_ACCESS_FLAG::OPEN_FILE;
				sint32 status;
				FSCVirtualFile* node = OpenNode(client, path, access, status);
				if (!node)
					return FSA_RESULT::NOT_FOUND;
				if (node->fscGetType() != FSC_TYPE_FILE)
				{
					delete node;
					return FSA_RESULT::NOT_FILE;
				}
				if (append)
					fsc_setFileSeek(node, fsc_getFileSize(node));
				FSResHandle handle;
				if (!g_openFiles.Insert(node, handle))
				{
					delete node;
					return FSA_RESULT::MAX_FILES;
				}
				shim->response.cmdOpenFile.fileHandleOutput = (uint32)handle;
				cemuLog_log(LogType::CoreinitFile, "Open file {} (access: {} result: ok handle: 0x{})", path, mode, (uint32)handle);
				return FSA_RESULT::OK;
			}

			FSA_RESULT CloseFile(FSAShimBuffer* shim)
			{
				const FSResHandle handle = (FSResHandle)(uint32)shim->request.cmdCloseFile.fileHandle;
				FSCVirtualFile* node = g_openFiles.Find(handle);
				if (!node)
				{
					cemuLog_logDebug(LogType::Force, "FSA CloseFile: unknown handle 0x{:08x}", (uint32)handle);
					return FSA_RESULT::INVALID_FILE_HANDLE;
				}
				g_openFiles.Erase(handle);
				fsc_close(node);
				return FSA_RESULT::OK;
			}

			FSA_RESULT OpenDir(Client& client, FSAShimBuffer* shim)
			{
				const char* path = (const char*)shim->request.cmdOpenDir.path;
				shim->response.cmdOpenDir.dirHandleOutput = (uint32)FS_INVALID_HANDLE_VALUE;
				sint32 status;
				FSCVirtualFile* node = OpenNode(client, path, FSC_ACCESS_FLAG::OPEN_DIR | FSC_ACCESS_FLAG::OPEN_FILE, status);
				if (!node)
					return FSA_RESULT::NOT_FOUND;
				if (node->fscGetType() != FSC_TYPE_DIRECTORY)
				{
					delete node;
					return FSA_RESULT::NOT_DIR;
				}
				FSResHandle handle;
				if (!g_openDirs.Insert(node, handle))
				{
					delete node;
					return FSA_RESULT::MAX_DIRS;
				}
				shim->response.cmdOpenDir.dirHandleOutput = (uint32)handle;
				cemuLog_log(LogType::CoreinitFile, "Open directory {} (result: ok handle: 0x{})", path, (uint32)handle);
				return FSA_RESULT::OK;
			}

			FSA_RESULT ReadDir(FSAShimBuffer* shim)
			{
				FSCVirtualFile* node = g_openDirs.Find((FSResHandle)(uint32)shim->request.cmdReadDir.dirHandle);
				if (!node)
					return FSA_RESULT::INVALID_DIR_HANDLE;
				FSCDirEntry entry;
				if (!fsc_nextDir(node, &entry))
					return FSA_RESULT::END_OF_DIRECTORY;
				// only name, flag, size and permissions are filled in
				FSDirEntry_t* out = &shim->response.cmdReadDir.dirEntry;
				strcpy(out->name, entry.path);
				FSFlag kind = FSFlag::NONE;
				out->stat.size = 0;
				if (entry.isDirectory)
					kind |= FSFlag::IS_DIR;
				else if (entry.isFile)
				{
					kind |= FSFlag::IS_FILE;
					out->stat.size = entry.fileSize;
				}
				out->stat.flag = kind;
				out->stat.permissions = 0x777;
				return FSA_RESULT::OK;
			}

			FSA_RESULT CloseDir(FSAShimBuffer* shim)
			{
				const FSResHandle handle = (FSResHandle)(uint32)shim->request.cmdCloseDir.dirHandle;
				FSCVirtualFile* node = g_openDirs.Find(handle);
				if (!node)
				{
					cemuLog_logDebug(LogType::Force, "FSA CloseDir: unknown handle 0x{:08x}", (uint32)handle);
					return FSA_RESULT::INVALID_DIR_HANDLE;
				}
				g_openDirs.Erase(handle);
				fsc_close(node);
				return FSA_RESULT::OK;
			}

			FSA_RESULT RewindDir(FSAShimBuffer* shim)
			{
				const FSResHandle handle = (FSResHandle)(uint32)shim->request.cmdRewindDir.dirHandle;
				FSCVirtualFile* node = g_openDirs.Find(handle);
				if (!node)
				{
					cemuLog_logDebug(LogType::Force, "FSA RewindDir: unknown handle 0x{:08x}", (uint32)handle);
					return FSA_RESULT::INVALID_DIR_HANDLE;
				}
				return node->fscRewindDir() ? FSA_RESULT::OK : FSA_RESULT::FATAL_ERROR;
			}

			FSA_RESULT Remove(Client& client, FSAShimBuffer* shim)
			{
				const std::string path = ResolvePath(client, (const char*)shim->request.cmdRemove.path);
				sint32 status = FSC_STATUS_FILE_NOT_FOUND;
				fsc_remove(path.c_str(), &status);
				return FromFscStatus(status);
			}

			FSA_RESULT MakeDir(Client& client, FSAShimBuffer* shim)
			{
				const std::string path = ResolvePath(client, (const char*)shim->request.cmdMakeDir.path);
				sint32 status = FSC_STATUS_FILE_NOT_FOUND;
				fsc_createDir(path.c_str(), &status);
				return FromFscStatus(status);
			}

			FSA_RESULT Rename(Client& client, FSAShimBuffer* shim)
			{
				const std::string from = ResolvePath(client, (const char*)shim->request.cmdRename.srcPath);
				const std::string to = ResolvePath(client, (const char*)shim->request.cmdRename.dstPath);
				sint32 status = FSC_STATUS_FILE_NOT_FOUND;
				fsc_rename(from.c_str(), to.c_str(), &status);
				return FromFscStatus(status);
			}

			FSA_RESULT ChangeDir(Client& client, FSAShimBuffer* shim)
			{
				auto& path = shim->request.cmdChangeDir.path;
				path[sizeof(path) - 1] = '\0';
				client.cwd = ResolvePath(client, (const char*)path, true);
				return FSA_RESULT::OK;
			}

			FSA_RESULT GetCwd(const Client& client, FSAShimBuffer* shim)
			{
				char* out = shim->response.cmdGetCWD.path;
				constexpr size_t capacity = sizeof(shim->response.cmdGetCWD.path);
				const size_t n = std::min(client.cwd.size() + 1, capacity);
				strncpy(out, client.cwd.c_str(), n);
				out[capacity - 1] = '\0';
				return FSA_RESULT::OK;
			}

			FSA_RESULT QueryInfo(Client& client, FSAShimBuffer* shim)
			{
				const char* path = (const char*)shim->request.cmdQueryInfo.query;
				switch ((uint32)shim->request.cmdQueryInfo.queryType)
				{
				case FSA_QUERY_TYPE_STAT:
					return StatPath(client, path, &shim->response.cmdQueryInfo.queryStat.stat);
				case FSA_QUERY_TYPE_FREESPACE:
				{
					sint32 status;
					FSCVirtualFile* node = OpenNode(client, path, FSC_ACCESS_FLAG::OPEN_FILE | FSC_ACCESS_FLAG::OPEN_DIR, status);
					if (!node)
						return FromFscStatus(status);
					shim->response.cmdQueryInfo.queryFreeSpace.freespace = 30ull * 1024 * 1024 * 1024; // fixed 30 GiB
					delete node;
					return FSA_RESULT::OK;
				}
				case FSA_QUERY_TYPE_DEVICE_INFO:
				{
					// fixed 32 GiB device with 512 byte sectors
					FSADeviceInfo_t& info = shim->response.cmdQueryInfo.queryDeviceInfo.info;
					info.deviceSectorSize = 512;
					info.deviceSizeInSectors = (32ull * 1024 * 1024 * 1024) / 512;
					cemu_assert_suspicious();
					return FSA_RESULT::OK;
				}
				default:
					cemu_assert_unimplemented();
					return FSA_RESULT::OK;
				}
			}

			FSA_RESULT StatOpenFile(FSAShimBuffer* shim)
			{
				FSCVirtualFile* node = g_openFiles.Find((FSResHandle)(uint32)shim->request.cmdGetStatFile.fileHandle);
				if (!node)
					return FSA_RESULT::NOT_FOUND;
				cemu_assert_debug(fsc_isFile(node));
				DescribeNode(node, &shim->response.cmdStatFile.statOut);
				return FSA_RESULT::OK;
			}

			FSA_RESULT ReadFile(FSAShimBuffer* shim, MEMPTR<void> dest, uint32 byteCount)
			{
				const auto& req = shim->request.cmdReadFile;
				const uint32 elementSize = req.size;
				FSCVirtualFile* node = g_openFiles.Find((FSResHandle)(uint32)req.fileHandle);
				if (!node)
					return FSA_RESULT::INVALID_FILE_HANDLE;
				if (((uint32)req.flag & FSA_CMD_FLAG_SET_POS) != 0)
					fsc_setFileSeek(node, req.filePos);
				const uint32 bytesRead = fsc_readFile(node, dest.GetPtr(), byteCount);
				if (elementSize == 0)
					return FSA_RESULT::OK;
				LatteBufferCache_notifyDCFlush(dest.GetMPTR(), byteCount);
				return (FSA_RESULT)(bytesRead / elementSize);
			}

			FSA_RESULT WriteFile(FSAShimBuffer* shim, MEMPTR<void> src, uint32 byteCount)
			{
				const auto& req = shim->request.cmdWriteFile;
				const uint32 elementSize = req.size;
				FSCVirtualFile* node = g_openFiles.Find((FSResHandle)(uint32)req.fileHandle);
				if (!node)
					return FSA_RESULT::INVALID_FILE_HANDLE;
				cemu_assert_debug(elementSize != 0 && (byteCount % elementSize) == 0);
				if (!fsc_isWritable(node))
				{
					cemu_assert_debug(false);
					return FSA_RESULT::PERMISSION_ERROR;
				}
				if (((uint32)req.flag & FSA_CMD_FLAG_SET_POS) != 0)
					fsc_setFileSeek(node, req.filePos);
				const uint32 bytesWritten = fsc_writeFile(node, src.GetPtr(), byteCount);
				if (elementSize == 0)
					return FSA_RESULT::OK;
				return (FSA_RESULT)(bytesWritten / elementSize);
			}

			FSA_RESULT SetPos(FSAShimBuffer* shim)
			{
				FSCVirtualFile* node = g_openFiles.Find((FSResHandle)(uint32)shim->request.cmdSetPosFile.fileHandle);
				if (!node)
					return FSA_RESULT::INVALID_FILE_HANDLE;
				fsc_setFileSeek(node, shim->request.cmdSetPosFile.filePos);
				return FSA_RESULT::OK;
			}

			FSA_RESULT GetPos(FSAShimBuffer* shim)
			{
				FSCVirtualFile* node = g_openFiles.Find((FSResHandle)(uint32)shim->request.cmdGetPosFile.fileHandle);
				if (!node)
					return FSA_RESULT::INVALID_FILE_HANDLE;
				shim->response.cmdGetPosFile.filePos = fsc_getFileSeek(node);
				return FSA_RESULT::OK;
			}

			FSA_RESULT IsEof(FSAShimBuffer* shim)
			{
				FSCVirtualFile* node = g_openFiles.Find((FSResHandle)(uint32)shim->request.cmdIsEof.fileHandle);
				if (!node)
					return FSA_RESULT::INVALID_FILE_HANDLE;
				return fsc_getFileSeek(node) >= fsc_getFileSize(node) ? FSA_RESULT::END_OF_FILE : FSA_RESULT::OK;
			}

			FSA_RESULT AppendFile(FSAShimBuffer* shim)
			{
				// space is not reserved; the call only reports success for every element
				if (!g_openFiles.Find((FSResHandle)(uint32)shim->request.cmdAppendFile.fileHandle))
					return FSA_RESULT::INVALID_FILE_HANDLE;
				return (FSA_RESULT)(uint32)shim->request.cmdAppendFile.count;
			}

			FSA_RESULT TruncateFile(FSAShimBuffer* shim)
			{
				FSCVirtualFile* node = g_openFiles.Find((FSResHandle)(uint32)shim->request.cmdTruncateFile.fileHandle);
				if (!node)
					return FSA_RESULT::INVALID_FILE_HANDLE;
				fsc_setFileLength(node, fsc_getFileSeek(node));
				return FSA_RESULT::OK;
			}

			FSA_RESULT RunIoctl(Client& client, FSA_CMD_OPERATION_TYPE op, FSAShimBuffer* shim)
			{
				using Op = FSA_CMD_OPERATION_TYPE;
				switch (op)
				{
				case Op::CHANGEDIR: return ChangeDir(client, shim);
				case Op::GETCWD: return GetCwd(client, shim);
				case Op::MAKEDIR: return MakeDir(client, shim);
				case Op::REMOVE: return Remove(client, shim);
				case Op::RENAME: return Rename(client, shim);
				case Op::OPENDIR: return OpenDir(client, shim);
				case Op::READDIR: return ReadDir(shim);
				case Op::REWINDDIR: return RewindDir(shim);
				case Op::CLOSEDIR: return CloseDir(shim);
				case Op::OPENFILE: return OpenFile(client, shim);
				case Op::GETPOS: return GetPos(shim);
				case Op::SETPOS: return SetPos(shim);
				case Op::ISEOF: return IsEof(shim);
				case Op::GETSTATFILE: return StatOpenFile(shim);
				case Op::CLOSEFILE: return CloseFile(shim);
				case Op::FLUSHFILE: return FSA_RESULT::OK;
				case Op::QUERYINFO: return QueryInfo(client, shim);
				case Op::APPENDFILE: return AppendFile(shim);
				case Op::TRUNCATEFILE: return TruncateFile(shim);
				case Op::FLUSHQUOTA: return FSA_RESULT::OK;
				case Op::READ:
				case Op::WRITE:
					cemu_assert_error(); // only valid as ioctlv
					return FSA_RESULT::FATAL_ERROR;
				default:
					return FSA_RESULT::FATAL_ERROR;
				}
			}

			FSA_RESULT RunIoctlv(FSA_CMD_OPERATION_TYPE op, IPCIoctlVector* vec)
			{
				// vec[0] is the shim buffer, vec[1] the data buffer
				switch (op)
				{
				case FSA_CMD_OPERATION_TYPE::READ:
					return ReadFile((FSAShimBuffer*)vec[0].basePhys.GetPtr(), vec[1].basePhys, vec[1].size);
				case FSA_CMD_OPERATION_TYPE::WRITE:
					return WriteFile((FSAShimBuffer*)vec[0].basePhys.GetPtr(), vec[1].basePhys, vec[1].size);
				case FSA_CMD_OPERATION_TYPE::CHANGEDIR:
				case FSA_CMD_OPERATION_TYPE::GETCWD:
				case FSA_CMD_OPERATION_TYPE::MAKEDIR:
				case FSA_CMD_OPERATION_TYPE::RENAME:
				case FSA_CMD_OPERATION_TYPE::OPENDIR:
				case FSA_CMD_OPERATION_TYPE::READDIR:
				case FSA_CMD_OPERATION_TYPE::CLOSEDIR:
				case FSA_CMD_OPERATION_TYPE::OPENFILE:
				case FSA_CMD_OPERATION_TYPE::REMOVE:
				case FSA_CMD_OPERATION_TYPE::GETPOS:
				case FSA_CMD_OPERATION_TYPE::SETPOS:
				case FSA_CMD_OPERATION_TYPE::ISEOF:
				case FSA_CMD_OPERATION_TYPE::GETSTATFILE:
				case FSA_CMD_OPERATION_TYPE::CLOSEFILE:
				case FSA_CMD_OPERATION_TYPE::QUERYINFO:
				case FSA_CMD_OPERATION_TYPE::APPENDFILE:
				case FSA_CMD_OPERATION_TYPE::TRUNCATEFILE:
				case FSA_CMD_OPERATION_TYPE::FLUSHQUOTA:
					cemu_assert_error(); // only valid as ioctl
					return FSA_RESULT::FATAL_ERROR;
				default:
					cemu_assert_unimplemented();
					return FSA_RESULT::FATAL_ERROR;
				}
			}

			/* ---- service thread ---- */

			IOSMsgQueueId g_queue;
			SysAllocator<iosu::kernel::IOSMessage, 352> g_queueStorage;
			std::thread g_thread;

			Client& ClientFor(uint32 handle)
			{
				cemu_assert(handle < g_clients.size());
				return g_clients[handle];
			}

			void ServiceLoop()
			{
				SetThreadName("IOSU-FSA");
				while (true)
				{
					IOSMessage msg;
					IOS_ERROR err = IOS_ReceiveMessage(g_queue, &msg, 0);
					cemu_assert(!IOS_ResultIsError(err));
					if (msg == 0)
						return; // shutdown request
					IPCCommandBody* cmd = MEMPTR<IPCCommandBody>(msg).GetPtr();
					const uint32 handle = cmd->devHandle;
					switch ((IPCCommandId)cmd->cmdId)
					{
					case IPCCommandId::IOS_OPEN:
					{
						IOS_ERROR reply = (IOS_ERROR)FSA_RESULT::MAX_CLIENTS;
						for (size_t i = 0; i < g_clients.size(); i++)
						{
							if (g_clients[i].open)
								continue;
							g_clients[i].open = true;
							g_clients[i].cwd = "/";
							reply = (IOS_ERROR)i;
							break;
						}
						IOS_ResourceReply(cmd, reply);
						break;
					}
					case IPCCommandId::IOS_CLOSE:
						ClientFor(handle).open = false;
						IOS_ResourceReply(cmd, IOS_ERROR_OK);
						break;
					case IPCCommandId::IOS_IOCTL:
					{
						Client& client = ClientFor(handle);
						cemu_assert(client.open);
						FSAShimBuffer* shim = MEMPTR<FSAShimBuffer>(cmd->args[1]).GetPtr();
						IOS_ResourceReply(cmd, (IOS_ERROR)RunIoctl(client, (FSA_CMD_OPERATION_TYPE)(uint32)cmd->args[0], shim));
						break;
					}
					case IPCCommandId::IOS_IOCTLV:
					{
						Client& client = ClientFor(handle);
						cemu_assert(client.open);
						IPCIoctlVector* vec = MEMPTR<IPCIoctlVector>(cmd->args[3]).GetPtr();
						IOS_ResourceReply(cmd, (IOS_ERROR)RunIoctlv((FSA_CMD_OPERATION_TYPE)(uint32)cmd->args[0], vec));
						break;
					}
					default:
						cemuLog_log(LogType::Force, "/dev/fsa: Unsupported IPC cmdId");
						cemu_assert_suspicious();
						IOS_ResourceReply(cmd, IOS_ERROR_INVALID);
						break;
					}
				}
			}
		}

		void Initialize()
		{
			for (Client& c : g_clients)
				c.open = false;
			g_queue = IOS_CreateMessageQueue(g_queueStorage.GetPtr(), g_queueStorage.GetCount());
			const IOS_ERROR err = IOS_RegisterResourceManager("/dev/fsa", g_queue);
			IOS_DeviceAssociateId("/dev/fsa", 11);
			cemu_assert(!IOS_ResultIsError(err));
			g_thread = std::thread(ServiceLoop);
		}

		void Shutdown()
		{
			IOS_SendMessage(g_queue, 0, 0);
			g_thread.join();
		}
	}
}
