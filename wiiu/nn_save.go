package wiiu

// nn_save, the save data library, and the parts of nn_act (accounts) games
// ask for before they save.
//
// A title's saves live at /vol/save: /vol/save/common for data every account
// shares and /vol/save/<persistent id> for each account's own. RetroPixels
// has one account, slot 1, whose persistent id is 80000001. The SAVE*
// functions are FS calls on paths inside an account's folder.

import "fmt"

const (
	actSlot         = 1
	actPersistentID = 0x80000001
)

// saveDir is the folder of an account slot (0xff = the shared one).
func saveDir(account uint32) string {
	if account&0xff == 0xff {
		return "/vol/save/common"
	}
	return fmt.Sprintf("/vol/save/%08x", 0x80000000|account&0xff)
}

func (m *Machine) saveInit() {
	for _, d := range []string{"/vol/save", saveDir(0xff), saveDir(actSlot)} {
		if _, _, err := m.Files.Stat(d); err != nil {
			m.Files.MakeDir(d)
		}
	}
}

func init() {
	s := "nn_save"
	ok := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) }
	reg(s, "SAVEInit", func(m *Machine, cpu *CPU, t *Thread) { m.saveInit(); ret(cpu, 0) })
	reg(s, "SAVEShutdown", ok)
	reg(s, "SAVEInitSaveDir", func(m *Machine, cpu *CPU, t *Thread) {
		m.saveInit()
		if d := saveDir(arg(cpu, 0)); !m.Files.exists(d) {
			m.Files.MakeDir(d)
		}
		ret(cpu, 0)
	})
	reg(s, "SAVEInitCommonSaveDir", func(m *Machine, cpu *CPU, t *Thread) { m.saveInit(); ret(cpu, 0) })
	reg(s, "SAVEInitAccountSaveDir", func(m *Machine, cpu *CPU, t *Thread) {
		m.saveInit()
		if d := saveDir(arg(cpu, 0)); !m.Files.exists(d) {
			m.Files.MakeDir(d)
		}
		ret(cpu, 0)
	})
	for _, n := range []string{"SAVEChangeGroupMode", "SAVEChangeGroupAndOthersMode", "SAVEChangeOtherMode"} {
		reg(s, n, ok)
	}

	// (client, block, account, ...): each with its Async twin, whose
	// FSAsyncData comes after the error mask (argument errArg)
	both := func(name string, errArg int, fn func(m *Machine, cpu *CPU, dir string) uint32) {
		reg(s, name, func(m *Machine, cpu *CPU, t *Thread) {
			m.saveInit()
			m.fsDone(cpu, arg(cpu, 0), arg(cpu, 1), fn(m, cpu, saveDir(arg(cpu, 2))), -1)
		})
		reg(s, name+"Async", func(m *Machine, cpu *CPU, t *Thread) {
			m.saveInit()
			m.fsDone(cpu, arg(cpu, 0), arg(cpu, 1), fn(m, cpu, saveDir(arg(cpu, 2))), errArg+1)
		})
	}
	in := func(m *Machine, cpu *CPU, dir string, i int) string {
		return cleanPath(dir + "/" + m.Mem.String(arg(cpu, i), 1024))
	}
	both("SAVEOpenFile", 6, func(m *Machine, cpu *CPU, dir string) uint32 {
		// (client, block, account, path, mode, handle*, errMask)
		h, st := m.fsOpen(in(m, cpu, dir, 3), m.Mem.String(arg(cpu, 4), 16))
		if st == fsOK {
			m.Mem.Write32(arg(cpu, 5), h)
		}
		return st
	})
	both("SAVEOpenDir", 5, func(m *Machine, cpu *CPU, dir string) uint32 {
		h, st := m.fsOpenDir(in(m, cpu, dir, 3))
		if st == fsOK {
			m.Mem.Write32(arg(cpu, 4), h)
		}
		return st
	})
	both("SAVEGetStat", 5, func(m *Machine, cpu *CPU, dir string) uint32 {
		return m.fsStat(in(m, cpu, dir, 3), arg(cpu, 4))
	})
	both("SAVEMakeDir", 4, func(m *Machine, cpu *CPU, dir string) uint32 {
		p := in(m, cpu, dir, 3)
		if m.Files.exists(p) {
			return fsExists
		}
		return errFS(m.Files.MakeDir(p))
	})
	both("SAVERemove", 4, func(m *Machine, cpu *CPU, dir string) uint32 {
		return errFS(m.Files.Remove(in(m, cpu, dir, 3)))
	})
	both("SAVERename", 5, func(m *Machine, cpu *CPU, dir string) uint32 {
		return errFS(m.Files.Rename(in(m, cpu, dir, 3), in(m, cpu, dir, 4)))
	})
	both("SAVEChangeDir", 4, func(m *Machine, cpu *CPU, dir string) uint32 {
		fs := m.fsInit()
		fs.Cwd[arg(cpu, 0)] = in(m, cpu, dir, 3)
		return fsOK
	})
	both("SAVEFlushQuota", 3, func(m *Machine, cpu *CPU, dir string) uint32 { return fsOK })
	both("SAVEGetFreeSpaceSize", 4, func(m *Machine, cpu *CPU, dir string) uint32 {
		m.Mem.Write64(arg(cpu, 3), 512<<20)
		return fsOK
	})
	// (title ID, file name, out, out size): the path of a file in a system
	// data title, such as the Mii parts. Those titles aren't here, so the
	// file won't open, but the program gets a proper path to try.
	// (title ID, file name, out, out size): a file in another title's
	// shared save data, which isn't here either
	reg(s, "SAVEGetSharedSaveDataPath", func(m *Machine, cpu *CPU, t *Thread) {
		id := arg64(cpu, 0)
		p := fmt.Sprintf("/vol/storage_mlc01/usr/save/%08x/%08x/user/common/%s", uint32(id>>32), uint32(id),
			m.Mem.String(arg(cpu, 2), 256))
		if out, size := arg(cpu, 3), arg(cpu, 4); out != 0 && size > 0 {
			if uint32(len(p)) >= size {
				p = p[:size-1]
			}
			m.Mem.WriteBytes(out, append([]byte(p), 0))
		}
		ret(cpu, fsOK)
	})
	reg(s, "SAVEGetSharedDataTitlePath", func(m *Machine, cpu *CPU, t *Thread) {
		id := arg64(cpu, 0)
		p := fmt.Sprintf("/vol/storage_mlc01/sys/title/%08x/%08x/content/%s", uint32(id>>32), uint32(id),
			m.Mem.String(arg(cpu, 2), 256))
		out, size := arg(cpu, 3), arg(cpu, 4)
		if out == 0 || size == 0 {
			ret(cpu, fsNotFound)
			return
		}
		if uint32(len(p)) >= size {
			p = p[:size-1]
		}
		m.Mem.WriteBytes(out, append([]byte(p), 0))
		ret(cpu, fsOK)
	})

	// nn_act: one account, signed in
	a := "nn_act"
	reg(a, "Initialize__Q2_2nn3actFv", ok)
	reg(a, "Finalize__Q2_2nn3actFv", ok)
	reg(a, "GetSlotNo__Q2_2nn3actFv", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, actSlot) })
	reg(a, "GetDefaultAccount__Q2_2nn3actFv", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, actSlot) })
	reg(a, "GetNumOfAccounts__Q2_2nn3actFv", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	reg(a, "IsSlotOccupied__Q2_2nn3actFUc", func(m *Machine, cpu *CPU, t *Thread) { retBool(cpu, arg(cpu, 0) == actSlot) })
	reg(a, "GetPersistentId__Q2_2nn3actFv", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, actPersistentID) })
	reg(a, "GetPersistentIdEx__Q2_2nn3actFUc", func(m *Machine, cpu *CPU, t *Thread) {
		if arg(cpu, 0) == actSlot {
			ret(cpu, actPersistentID)
		} else {
			ret(cpu, 0)
		}
	})
	reg(a, "GetPrincipalId__Q2_2nn3actFv", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(a, "IsNetworkAccount__Q2_2nn3actFv", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) })
	reg(a, "IsCommitted__Q2_2nn3actFv", func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) })
	accountID := func(m *Machine, out uint32) {
		m.Mem.Fill(out, 0, 17)
		m.Mem.WriteString(out, "RetroPixels")
	}
	reg(a, "GetAccountId__Q2_2nn3actFPc", func(m *Machine, cpu *CPU, t *Thread) { accountID(m, arg(cpu, 0)); ret(cpu, 0) })
	reg(a, "GetAccountIdEx__Q2_2nn3actFPcUc", func(m *Machine, cpu *CPU, t *Thread) { accountID(m, arg(cpu, 0)); ret(cpu, 0) })
	// the Mii's name: UTF-16, 10 characters and a 0
	miiName := func(m *Machine, out uint32) {
		m.Mem.Fill(out, 0, 22)
		for i, r := range "Player" {
			m.Mem.Write16(out+uint32(i)*2, uint16(r))
		}
	}
	reg(a, "GetMiiName__Q2_2nn3actFPw", func(m *Machine, cpu *CPU, t *Thread) { miiName(m, arg(cpu, 0)); ret(cpu, 0) })
	reg(a, "GetMiiNameEx__Q2_2nn3actFPwUc", func(m *Machine, cpu *CPU, t *Thread) { miiName(m, arg(cpu, 0)); ret(cpu, 0) })
	reg(a, "GetTransferableId__Q2_2nn3actFUi", func(m *Machine, cpu *CPU, t *Thread) { ret64(cpu, 0x1234567890abcdef) })
	reg(a, "GetTransferableIdEx__Q2_2nn3actFPULUiUc", func(m *Machine, cpu *CPU, t *Thread) {
		m.Mem.Write64(arg(cpu, 0), 0x1234567890abcdef)
		ret(cpu, 0)
	})
}
