// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// LEGO Dimensions toy pad protocol: the game writes 32-byte commands
// (0x55, length, command, sequence, payload..., checksum) and reads 32-byte
// replies (0x55, length, sequence, payload..., checksum). Tags placed on or
// lifted from the pad are announced with 0x56 reports once the pad has been
// woken through the challenge exchange. Challenge payloads are TEA
// encrypted; tags are NTAG213 images of 45 four-byte pages.
#include "Dimensions.h"

#include <random>

namespace nsyshid
{
	DimensionsUSB g_dimensionstoypad;

	// ---- protocol arithmetic
	namespace dimensions
	{
		// TEA key of the command channel
		constexpr uint8 kCommandKey[16] = {0x55, 0xFE, 0xF6, 0xB0, 0x62, 0xBF, 0x0B, 0x41,
										   0xC9, 0xB3, 0x7C, 0xB4, 0x97, 0x3E, 0x29, 0x7B};
		// appended to the UID when deriving a tag's key
		constexpr uint8 kKeySalt[17] = {0xB7, 0xD5, 0xD7, 0xE6, 0xE7, 0xBA, 0x3C, 0xA8, 0xD8,
										0x75, 0x47, 0x68, 0xCF, 0x23, 0xE9, 0xFE, 0xAA};
		// appended to the UID when deriving a tag's password
		constexpr char kPasswordSalt[] = "(c) Copyright LEGO 2014";

		uint32 LE32(const uint8* p)
		{
			return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
		}

		void PutLE32(uint8* p, uint32 v)
		{
			for (int i = 0; i < 4; i++)
				p[i] = (uint8)(v >> (8 * i));
		}

		uint8 Checksum(const uint8* data, size_t count)
		{
			uint32 sum = 0;
			for (size_t i = 0; i < count; i++)
				sum += data[i];
			return (uint8)sum;
		}

		// TEA, 32 cycles, block and key as little-endian words
		std::array<uint8, 8> TeaEncrypt(const uint8* block, const uint8* key)
		{
			uint32 v0 = LE32(block), v1 = LE32(block + 4);
			const uint32 k0 = LE32(key), k1 = LE32(key + 4), k2 = LE32(key + 8), k3 = LE32(key + 12);
			uint32 sum = 0;
			for (int i = 0; i < 32; i++)
			{
				sum += 0x9E3779B9;
				v0 += ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
				v1 += ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
			}
			std::array<uint8, 8> out;
			PutLE32(out.data(), v0);
			PutLE32(out.data() + 4, v1);
			return out;
		}

		std::array<uint8, 8> TeaDecrypt(const uint8* block, const uint8* key)
		{
			uint32 v0 = LE32(block), v1 = LE32(block + 4);
			const uint32 k0 = LE32(key), k1 = LE32(key + 4), k2 = LE32(key + 8), k3 = LE32(key + 12);
			uint32 sum = 0x9E3779B9u * 32;
			for (int i = 0; i < 32; i++)
			{
				v1 -= ((v0 << 4) + k2) ^ (v0 + sum) ^ ((v0 >> 5) + k3);
				v0 -= ((v1 << 4) + k0) ^ (v1 + sum) ^ ((v1 >> 5) + k1);
				sum -= 0x9E3779B9;
			}
			std::array<uint8, 8> out;
			PutLE32(out.data(), v0);
			PutLE32(out.data() + 4, v1);
			return out;
		}

		// generator in the style of Jenkins' small fast PRNG (rotations 21, 19, 6)
		uint32 RngNext(uint32& a, uint32& b, uint32& c, uint32& d)
		{
			const uint32 e = a - std::rotl(b, 21);
			a = b ^ std::rotl(c, 19);
			b = c + std::rotl(d, 6);
			c = d + e;
			d = e + a;
			return d;
		}

		void RngSeed(uint32& a, uint32& b, uint32& c, uint32& d, uint32 seed)
		{
			a = 0xF1EA5EED;
			b = c = d = seed;
			for (int i = 0; i < 42; i++)
				RngNext(a, b, c, d);
		}

		// folds `words` little-endian words of `data` into one value
		uint32 Fold(const uint8* data, int words)
		{
			uint32 v = 0;
			for (int i = 0; i < words; i++)
				v = LE32(data + 4 * i) + std::rotr(v, 25) + std::rotr(v, 10) - v;
			return v;
		}

		// the 7 UID bytes of a tag image (page 0 bytes 0-2, page 1)
		std::array<uint8, 7> Uid(const uint8* tag)
		{
			return {tag[0], tag[1], tag[2], tag[4], tag[5], tag[6], tag[7]};
		}

		std::array<uint8, 16> TagKey(const uint8* tag)
		{
			const std::array<uint8, 7> uid = Uid(tag);
			std::array<uint8, 16> key{};
			for (int part = 0; part < 4; part++)
			{
				const int words = 3 + part;
				uint8 buf[24];
				memcpy(buf, uid.data(), 7);
				memcpy(buf + 7, kKeySalt, sizeof(kKeySalt));
				buf[words * 4 - 1] = 0xAA;
				PutLE32(&key[part * 4], Fold(buf, words));
			}
			return key;
		}

		std::array<uint8, 4> TagPassword(const uint8* tag)
		{
			const std::array<uint8, 7> uid = Uid(tag);
			uint8 buf[32];
			memcpy(buf, uid.data(), 7);
			memcpy(buf + 7, kPasswordSalt, sizeof(kPasswordSalt) - 1);
			buf[30] = 0xAA;
			buf[31] = 0xAA;
			std::array<uint8, 4> pwd;
			PutLE32(pwd.data(), Fold(buf, 8));
			return pwd;
		}

		// characters store their id encrypted in pages 36-37; vehicles and
		// gadgets store it in plain text in page 36
		uint32 TagId(const uint8* tag)
		{
			const std::array<uint8, 16> key = TagKey(tag);
			const std::array<uint8, 8> plain = TeaDecrypt(tag + 36 * 4, key.data());
			const uint32 id = LE32(plain.data());
			return id < 1000 ? id : LE32(tag + 36 * 4);
		}
	}
	// ---- device

	namespace
	{
		struct DimensionsName
		{
			uint32 id;
			const char* name;
		};

		constexpr DimensionsName kMinifigNames[] = {
		{0, "Blank Tag"},
		{1, "Batman"},
		{2, "Gandalf"},
		{3, "Wyldstyle"},
		{4, "Aquaman"},
		{5, "Bad Cop"},
		{6, "Bane"},
		{7, "Bart Simpson"},
		{8, "Benny"},
		{9, "Chell"},
		{10, "Cole"},
		{11, "Cragger"},
		{12, "Cyborg"},
		{13, "Cyberman"},
		{14, "Doc Brown"},
		{15, "The Doctor"},
		{16, "Emmet"},
		{17, "Eris"},
		{18, "Gimli"},
		{19, "Gollum"},
		{20, "Harley Quinn"},
		{21, "Homer Simpson"},
		{22, "Jay"},
		{23, "Joker"},
		{24, "Kai"},
		{25, "ACU Trooper"},
		{26, "Gamer Kid"},
		{27, "Krusty the Clown"},
		{28, "Laval"},
		{29, "Legolas"},
		{30, "Lloyd"},
		{31, "Marty McFly"},
		{32, "Nya"},
		{33, "Owen Grady"},
		{34, "Peter Venkman"},
		{35, "Slimer"},
		{36, "Scooby-Doo"},
		{37, "Sensei Wu"},
		{38, "Shaggy"},
		{39, "Stay Puft"},
		{40, "Superman"},
		{41, "Unikitty"},
		{42, "Wicked Witch of the West"},
		{43, "Wonder Woman"},
		{44, "Zane"},
		{45, "Green Arrow"},
		{46, "Supergirl"},
		{47, "Abby Yates"},
		{48, "Finn the Human"},
		{49, "Ethan Hunt"},
		{50, "Lumpy Space Princess"},
		{51, "Jake the Dog"},
		{52, "Harry Potter"},
		{53, "Lord Voldemort"},
		{54, "Michael Knight"},
		{55, "B.A. Baracus"},
		{56, "Newt Scamander"},
		{57, "Sonic the Hedgehog"},
		{58, "Future Update (unreleased)"},
		{59, "Gizmo"},
		{60, "Stripe"},
		{61, "E.T."},
		{62, "Tina Goldstein"},
		{63, "Marceline the Vampire Queen"},
		{64, "Batgirl"},
		{65, "Robin"},
		{66, "Sloth"},
		{67, "Hermione Granger"},
		{68, "Chase McCain"},
		{69, "Excalibur Batman"},
		{70, "Raven"},
		{71, "Beast Boy"},
		{72, "Betelgeuse"},
		{73, "Lord Vortech (unreleased)"},
		{74, "Blossom"},
		{75, "Bubbles"},
		{76, "Buttercup"},
		{77, "Starfire"},
		{78, "World 15 (unreleased)"},
		{79, "World 16 (unreleased)"},
		{80, "World 17 (unreleased)"},
		{81, "World 18 (unreleased)"},
		{82, "World 19 (unreleased)"},
		{83, "World 20 (unreleased)"},
		{768, "Unknown 768"},
		{769, "Supergirl Red Lantern"},
		{770, "Unknown 770"},
		};

		constexpr DimensionsName kTokenNames[] = {
		{1000, "Police Car"},
		{1001, "Aerial Squad Car"},
		{1002, "Missile Striker"},
		{1003, "Gravity Sprinter"},
		{1004, "Street Shredder"},
		{1005, "Sky Clobberer"},
		{1006, "Batmobile"},
		{1007, "Batblaster"},
		{1008, "Sonic Batray"},
		{1009, "Benny's Spaceship"},
		{1010, "Lasercraft"},
		{1011, "The Annihilator"},
		{1012, "DeLorean Time Machine"},
		{1013, "Electric Time Machine"},
		{1014, "Ultra Time Machine"},
		{1015, "Hoverboard"},
		{1016, "Cyclone Board"},
		{1017, "Ultimate Hoverjet"},
		{1018, "Eagle Interceptor"},
		{1019, "Eagle Sky Blazer"},
		{1020, "Eagle Swoop Diver"},
		{1021, "Swamp Skimmer"},
		{1022, "Cragger's Fireship"},
		{1023, "Croc Command Sub"},
		{1024, "Cyber-Guard"},
		{1025, "Cyber-Wrecker"},
		{1026, "Laser Robot Walker"},
		{1027, "K-9"},
		{1028, "K-9 Ruff Rover"},
		{1029, "K-9 Laser Cutter"},
		{1030, "TARDIS"},
		{1031, "Laser-Pulse TARDIS"},
		{1032, "Energy-Burst TARDIS"},
		{1033, "Emmet's Excavator"},
		{1034, "Destroy Dozer"},
		{1035, "Destruct-o-Mech"},
		{1036, "Winged Monkey"},
		{1037, "Battle Monkey"},
		{1038, "Commander Monkey"},
		{1039, "Axe Chariot"},
		{1040, "Axe Hurler"},
		{1041, "Soaring Chariot"},
		{1042, "Shelob the Great"},
		{1043, "8-Legged Stalker"},
		{1044, "Poison Slinger"},
		{1045, "Homer's Car"},
		{1046, "The Homercraft"},
		{1047, "The SubmaHomer"},
		{1048, "Taunt-o-Vision"},
		{1049, "Blast Cam"},
		{1050, "The MechaHomer"},
		{1051, "Velociraptor"},
		{1052, "Spike Attack Raptor"},
		{1053, "Venom Raptor"},
		{1054, "Gyrosphere"},
		{1055, "Sonic Beam Gyrosphere"},
		{1056, " Gyrosphere"},
		{1057, "Clown Bike"},
		{1058, "Cannon Bike"},
		{1059, "Anti-Gravity Rocket Bike"},
		{1060, "Mighty Lion Rider"},
		{1061, "Lion Blazer"},
		{1062, "Fire Lion"},
		{1063, "Arrow Launcher"},
		{1064, "Seeking Shooter"},
		{1065, "Triple Ballista"},
		{1066, "Mystery Machine"},
		{1067, "Mystery Tow & Go"},
		{1068, "Mystery Monster"},
		{1069, "Boulder Bomber"},
		{1070, "Boulder Blaster"},
		{1071, "Cyclone Jet"},
		{1072, "Storm Fighter"},
		{1073, "Lightning Jet"},
		{1074, "Electro-Shooter"},
		{1075, "Blade Bike"},
		{1076, "Flight Fire Bike"},
		{1077, "Blades of Fire"},
		{1078, "Samurai Mech"},
		{1079, "Samurai Shooter"},
		{1080, "Soaring Samurai Mech"},
		{1081, "Companion Cube"},
		{1082, "Laser Deflector"},
		{1083, "Gold Heart Emitter"},
		{1084, "Sentry Turret"},
		{1085, "Turret Striker"},
		{1086, "Flight Turret Carrier"},
		{1087, "Scooby Snack"},
		{1088, "Scooby Fire Snack"},
		{1089, "Scooby Ghost Snack"},
		{1090, "Cloud Cuckoo Car"},
		{1091, "X-Stream Soaker"},
		{1092, "Rainbow Cannon"},
		{1093, "Invisible Jet"},
		{1094, "Laser Shooter"},
		{1095, "Torpedo Bomber"},
		{1096, "NinjaCopter"},
		{1097, "Glaciator"},
		{1098, "Freeze Fighter"},
		{1099, "Travelling Time Train"},
		{1100, "Flight Time Train"},
		{1101, "Missile Blast Time Train"},
		{1102, "Aqua Watercraft"},
		{1103, "Seven Seas Speeder"},
		{1104, "Trident of Fire"},
		{1105, "Drill Driver"},
		{1106, "Bane Dig 'n' Drill"},
		{1107, "Bane Drill 'n' Blast"},
		{1108, "Quinn Mobile"},
		{1109, "Quinn Ultra Racer"},
		{1110, "Missile Launcher"},
		{1111, "The Joker's Chopper"},
		{1112, "Mischievous Missile Blaster"},
		{1113, "Lock 'n' Laser Jet"},
		{1114, "Hover Pod"},
		{1115, "Krypton Striker"},
		{1116, "Super Stealth Pod"},
		{1117, "Dalek"},
		{1118, "Fire 'n' Ride Dalek"},
		{1119, "Silver Shooter Dalek"},
		{1120, "Ecto-1"},
		{1121, "Ecto-1 Blaster"},
		{1122, "Ecto-1 Water Diver"},
		{1123, "Ghost Trap"},
		{1124, "Ghost Stun 'n' Trap"},
		{1125, "Proton Zapper"},
		{1126, "Unknown"},
		{1127, "Unknown"},
		{1128, "Unknown"},
		{1129, "Unknown"},
		{1130, "Unknown"},
		{1131, "Unknown"},
		{1132, "Lloyd's Golden Dragon"},
		{1133, "Sword Projector Dragon"},
		{1134, "Unknown"},
		{1135, "Unknown"},
		{1136, "Unknown"},
		{1137, "Unknown"},
		{1138, "Unknown"},
		{1139, "Unknown"},
		{1140, "Unknown"},
		{1141, "Unknown"},
		{1142, "Unknown"},
		{1143, "Unknown"},
		{1144, "Mega Flight Dragon"},
		{1145, "Unknown"},
		{1146, "Unknown"},
		{1147, "Unknown"},
		{1148, "Unknown"},
		{1149, "Unknown"},
		{1150, "Unknown"},
		{1151, "Unknown"},
		{1152, "Unknown"},
		{1153, "Unknown"},
		{1154, "Unknown"},
		{1155, "Flying White Dragon"},
		{1156, "Golden Fire Dragon"},
		{1157, "Ultra Destruction Dragon"},
		{1158, "Arcade Machine"},
		{1159, "8-Bit Shooter"},
		{1160, "The Pixelator"},
		{1161, "G-6155 Spy Hunter"},
		{1162, "Interdiver"},
		{1163, "Aerial Spyhunter"},
		{1164, "Slime Shooter"},
		{1165, "Slime Exploder"},
		{1166, "Slime Streamer"},
		{1167, "Terror Dog"},
		{1168, "Terror Dog Destroyer"},
		{1169, "Soaring Terror Dog"},
		{1170, "Ancient Psychic Tandem War Elephant"},
		{1171, "Cosmic Squid"},
		{1172, "Psychic Submarine"},
		{1173, "BMO"},
		{1174, "DOGMO"},
		{1175, "SNAKEMO"},
		{1176, "Jakemobile"},
		{1177, "Snail Dude Jake"},
		{1178, "Hover Jake"},
		{1179, "Lumpy Car"},
		{1180, "Lumpy Truck"},
		{1181, "Lumpy Land Whale"},
		{1182, "Lunatic Amp"},
		{1183, "Shadow Scorpion"},
		{1184, "Heavy Metal Monster"},
		{1185, "B.A.'s Van"},
		{1186, "Fool Smasher"},
		{1187, "Pain Plane"},
		{1188, "Phone Home"},
		{1189, "Mobile Uplink"},
		{1190, "Super-Charged Satellite"},
		{1191, "Niffler"},
		{1192, "Sinister Scorpion"},
		{1193, "Vicious Vulture"},
		{1194, "Swooping Evil"},
		{1195, "Brutal Bloom"},
		{1196, "Crawling Creeper"},
		{1197, "Ecto-1 (2016)"},
		{1198, "Ectozer"},
		{1199, "PerfEcto"},
		{1200, "Flash 'n' Finish"},
		{1201, "Rampage Record Player"},
		{1202, "Stripe's Throne"},
		{1203, "R.C. Racer"},
		{1204, "Gadget-O-Matic"},
		{1205, "Scarlet Scorpion"},
		{1206, "Hogwarts Express"},
		{1207, "Soaring Steam Plane"},
		{1208, "Steam Warrior"},
		{1209, "Enchanted Car"},
		{1210, "Shark Sub"},
		{1211, "Monstrous Mouth"},
		{1212, "IMF Scrambler"},
		{1213, "Shock Cycle"},
		{1214, "IMF Covert Jet"},
		{1215, "IMF Sports Car"},
		{1216, "IMF Tank"},
		{1217, "IMF Splorer"},
		{1218, "Sonic Speedster"},
		{1219, "Blue Typhoon"},
		{1220, "Moto Bug"},
		{1221, "The Tornado"},
		{1222, "Crabmeat"},
		{1223, "Eggcatcher"},
		{1224, "K.I.T.T."},
		{1225, "Goliath Armored Semi"},
		{1226, "K.I.T.T. Jet"},
		{1227, "Police Helicopter"},
		{1228, "Police Hovercraft"},
		{1229, "Police Plane"},
		{1230, "Bionic Steed"},
		{1231, "Bat-Raptor"},
		{1232, "Ultrabat"},
		{1233, "Batwing"},
		{1234, "The Black Thunder"},
		{1235, "Bat-Tank"},
		{1236, "Skeleton Organ"},
		{1237, "Skeleton Jukebox"},
		{1238, "Skele-Turkey"},
		{1239, "One-Eyed Willy's Pirate Ship"},
		{1240, "Fanged Fortune"},
		{1241, "Inferno Cannon"},
		{1242, "Buckbeak"},
		{1243, "Giant Owl"},
		{1244, "Fierce Falcon"},
		{1245, "Saturn's Sandworm"},
		{1246, "Spooky Spider"},
		{1247, "Haunted Vacuum"},
		{1248, "PPG Smartphone"},
		{1249, "PPG Hotline"},
		{1250, "Powerpuff Mag-Net"},
		{1251, "Ka-Pow Cannon"},
		{1252, "Slammin' Guitar"},
		{1253, "Mega Blast Bot"},
		{1254, "Octi"},
		{1255, "Super Skunk"},
		{1256, "Sonic Squid"},
		{1257, "T-Car"},
		{1258, "T-Forklift"},
		{1259, "T-Plane"},
		{1260, "Spellbook of Azarath"},
		{1261, "Raven Wings"},
		{1262, "Giant Hand"},
		{1263, "Titan Robot"},
		{1264, "T-Rocket"},
		{1265, "Robot Retriever"},
		};

		std::map<const uint32, const char*> ToMap(std::span<const DimensionsName> names)
		{
			std::map<const uint32, const char*> list;
			for (const DimensionsName& e : names)
				list.emplace(e.id, e.name);
			return list;
		}
	}

	DimensionsToypadDevice::DimensionsToypadDevice()
		: EmulatedHidDevice(0x0E6F, 0x0241)
	{
	}

	Device::Status DimensionsToypadDevice::Read(uint8* buffer, uint32 length, sint32& bytesRead)
	{
		DimensionsUSB::Report r = g_dimensionstoypad.GetStatus();
		memcpy(buffer, r.data(), std::min<size_t>(length, r.size()));
		bytesRead = (sint32)length;
		return Status::Ok;
	}

	Device::Status DimensionsToypadDevice::Write(uint8* data, uint32 length, sint32& bytesWritten)
	{
		if (length != 32)
			return Status::Error;
		g_dimensionstoypad.SendCommand(std::span<const uint8, 32>{data, 32});
		bytesWritten = (sint32)length;
		return Status::Ok;
	}

	// ---- toy pad state --------------------------------------------------------------

	void DimensionsUSB::Rng::Seed(uint32 seed)
	{
		dimensions::RngSeed(a, b, c, d, seed);
	}

	uint32 DimensionsUSB::Rng::Next()
	{
		return dimensions::RngNext(a, b, c, d);
	}

	// blocks until a reply or (once awake) a tag event is available; replies first
	DimensionsUSB::Report DimensionsUSB::GetStatus()
	{
		while (true)
		{
			{
				std::unique_lock _l(m_lock);
				std::queue<Report>* source = !m_replies.empty() ? &m_replies : (m_awake && !m_tagEvents.empty()) ? &m_tagEvents : nullptr;
				if (source)
				{
					Report r = source->front();
					source->pop();
					return r;
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	void DimensionsUSB::SendCommand(std::span<const uint8, 32> buf)
	{
		const uint8 command = buf[2];
		const uint8 seq = buf[3];
		const std::span<const uint8, 8> payload8{buf.data() + 4, 8};

		std::unique_lock _l(m_lock);
		Report reply{};
		switch (command)
		{
		case 0xB0: // wake: the pad identifies itself
			reply = {0x55, 0x0E, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4C, 0x45, 0x47, 0x4F, 0x20, 0x32, 0x30, 0x31, 0x34, 0x46};
			break;
		case 0xB1:
			Seed(payload8, seq, reply);
			break;
		case 0xB3:
			Challenge(payload8, seq, reply);
			break;
		case 0xC0: // lights: acknowledged only
		case 0xC1:
		case 0xC2:
		case 0xC3:
		case 0xC4:
		case 0xC6:
		case 0xC7:
		case 0xC8:
			reply[0] = 0x55;
			reply[1] = 0x01;
			reply[2] = seq;
			reply[3] = dimensions::Checksum(reply.data(), 3);
			break;
		case 0xD2:
			ReadPages(buf[4], buf[5], seq, reply);
			break;
		case 0xD3:
			WritePage(buf[4], buf[5], std::span<const uint8, 4>{buf.data() + 6, 4}, seq, reply);
			break;
		case 0xD4:
			Model(payload8, seq, reply);
			break;
		case 0xD0: // tag list
		case 0xE1: // password
		case 0xE5: // active
		case 0xFF: // light query
			cemuLog_log(LogType::Force, "Dimensions toy pad: command 0x{:02x} not implemented", command);
			break;
		default:
			cemuLog_log(LogType::Force, "Dimensions toy pad: unknown command 0x{:02x}", command);
			break;
		}
		m_replies.push(reply);
	}

	static void PutEncryptedReply(uint8 seq, const std::array<uint8, 8>& plain, DimensionsUSB::Report& reply)
	{
		const std::array<uint8, 8> enc = dimensions::TeaEncrypt(plain.data(), dimensions::kCommandKey);
		reply[0] = 0x55;
		reply[1] = 0x09;
		reply[2] = seq;
		memcpy(&reply[3], enc.data(), 8);
		reply[11] = dimensions::Checksum(reply.data(), 11);
	}

	// payload: seed (LE) and a confirmation word that is echoed back
	void DimensionsUSB::Seed(std::span<const uint8, 8> payload, uint8 seq, Report& reply)
	{
		const std::array<uint8, 8> plain = dimensions::TeaDecrypt(payload.data(), dimensions::kCommandKey);
		m_rng.Seed(dimensions::LE32(plain.data()));
		PutEncryptedReply(seq, {plain[4], plain[5], plain[6], plain[7], 0, 0, 0, 0}, reply);
	}

	// payload: confirmation word; answer: next random number (LE) and the confirmation
	void DimensionsUSB::Challenge(std::span<const uint8, 8> payload, uint8 seq, Report& reply)
	{
		const std::array<uint8, 8> plain = dimensions::TeaDecrypt(payload.data(), dimensions::kCommandKey);
		std::array<uint8, 8> answer{};
		dimensions::PutLE32(answer.data(), m_rng.Next());
		memcpy(&answer[4], plain.data(), 4);
		PutEncryptedReply(seq, answer, reply);
		m_awake = true;
	}

	// four pages starting at `page` of the tag at 1-based `index`
	void DimensionsUSB::ReadPages(uint8 index, uint8 page, uint8 seq, Report& reply)
	{
		reply[0] = 0x55;
		reply[1] = 0x12;
		reply[2] = seq;
		reply[3] = 0x00;
		const uint8 slot = (uint8)(index - 1);
		if (slot < m_tags.size())
		{
			const Tag& t = m_tags[slot];
			if (t.Present() && 4 * page < (int)t.data.size() - 16)
				memcpy(&reply[4], t.data.data() + 4 * page, 16);
		}
		reply[20] = dimensions::Checksum(reply.data(), 20);
	}

	void DimensionsUSB::WritePage(uint8 index, uint8 page, std::span<const uint8, 4> data, uint8 seq, Report& reply)
	{
		reply[0] = 0x55;
		reply[1] = 0x02;
		reply[2] = seq;
		reply[3] = 0x00;
		const uint8 slot = (uint8)(index - 1);
		if (slot < m_tags.size())
		{
			Tag& t = m_tags[slot];
			if (t.Present() && page < 0x2D)
			{
				if (page == 36) // the id page
					t.id = dimensions::LE32(data.data());
				memcpy(t.data.data() + page * 4, data.data(), 4);
				t.Flush();
			}
		}
		reply[4] = dimensions::Checksum(reply.data(), 4);
	}

	// payload: 1-based index and a confirmation word; answer: the tag's id and the confirmation
	void DimensionsUSB::Model(std::span<const uint8, 8> payload, uint8 seq, Report& reply)
	{
		const std::array<uint8, 8> plain = dimensions::TeaDecrypt(payload.data(), dimensions::kCommandKey);
		std::array<uint8, 8> answer{};
		const uint8 slot = (uint8)(plain[0] - 1);
		if (slot < m_tags.size())
		{
			dimensions::PutLE32(answer.data(), m_tags[slot].id);
			memcpy(&answer[4], &plain[4], 4);
		}
		const std::array<uint8, 8> enc = dimensions::TeaEncrypt(answer.data(), dimensions::kCommandKey);
		reply[0] = 0x55;
		reply[1] = 0x0A;
		reply[2] = seq;
		reply[3] = 0x00;
		memcpy(&reply[4], enc.data(), 8);
		reply[12] = dimensions::Checksum(reply.data(), 12);
	}

	void DimensionsUSB::QueueTagEvent(const Tag& t, bool removed)
	{
		const std::array<uint8, 7> uid = dimensions::Uid(t.data.data());
		Report r{0x56, 0x0B, t.pad, 0x00, t.index, (uint8)(removed ? 1 : 0)};
		memcpy(&r[6], uid.data(), uid.size());
		r[13] = dimensions::Checksum(r.data(), 13);
		m_tagEvents.push(r);
	}

	uint32 DimensionsUSB::PlaceLocked(const TagData& buf, std::unique_ptr<FileStream> file, uint8 pad, uint8 index)
	{
		if (index >= m_tags.size())
			return 0;
		const uint32 id = dimensions::TagId(buf.data());
		Tag& t = m_tags[index];
		t.file = std::move(file);
		t.data = buf;
		t.id = id;
		t.pad = pad;
		t.index = index + 1;
		QueueTagEvent(t, false);
		return id;
	}

	// takes the tag off its slot; `announce` also tells the game and closes the file
	bool DimensionsUSB::LiftLocked(uint8 index, bool announce)
	{
		if (index >= m_tags.size())
			return false;
		Tag& t = m_tags[index];
		if (!t.Present())
			return false;
		if (announce)
		{
			QueueTagEvent(t, true);
			t.Flush();
			t.file.reset();
		}
		t.index = 255;
		t.pad = 255;
		t.id = 0;
		return true;
	}

	uint32 DimensionsUSB::LoadFigure(const TagData& buf, std::unique_ptr<FileStream> file, uint8 pad, uint8 index)
	{
		std::unique_lock _l(m_lock);
		return PlaceLocked(buf, std::move(file), pad, index);
	}

	bool DimensionsUSB::RemoveFigure(uint8 pad, uint8 index, bool fullRemove)
	{
		std::unique_lock _l(m_lock);
		return LiftLocked(index, fullRemove);
	}

	// the user picked a tag up to move it: the game sees it leave
	bool DimensionsUSB::TempRemove(uint8 index)
	{
		std::unique_lock _l(m_lock);
		if (index >= m_tags.size() || !m_tags[index].Present())
			return false;
		QueueTagEvent(m_tags[index], true);
		return true;
	}

	// the move was cancelled: the tag comes back
	bool DimensionsUSB::CancelRemove(uint8 index)
	{
		std::unique_lock _l(m_lock);
		if (index >= m_tags.size() || !m_tags[index].Present())
			return false;
		QueueTagEvent(m_tags[index], false);
		return true;
	}

	bool DimensionsUSB::MoveFigure(uint8 pad, uint8 index, uint8 oldPad, uint8 oldIndex)
	{
		if (oldIndex == index)
		{
			// put back where it was picked up
			CancelRemove(index);
			return true;
		}
		std::unique_lock _l(m_lock);
		if (oldIndex >= m_tags.size() || index >= m_tags.size())
			return false;
		// whatever stands on the target slot is taken off first
		LiftLocked(index, true);
		Tag& from = m_tags[oldIndex];
		const TagData data = from.data;
		std::unique_ptr<FileStream> file = std::move(from.file);
		LiftLocked(oldIndex, false);
		PlaceLocked(data, std::move(file), pad, index);
		return true;
	}

	bool DimensionsUSB::CreateFigure(fs::path pathName, uint32 id)
	{
		std::unique_ptr<FileStream> out(FileStream::createFile2(pathName));
		if (!out)
			return false;

		TagData tag{};
		// UID: manufacturer 0x04, random serial; byte 7 is fixed
		std::random_device rd;
		std::uniform_int_distribution<int> byte(0, 255);
		tag[0] = 0x04;
		for (int i : {1, 2, 4, 5, 6})
			tag[i] = (uint8)byte(rd);
		tag[7] = 0x80;
		tag[3] = (uint8)id;

		if (id != 0)
		{
			// characters: encrypted id twice in pages 36-37, password in page 43
			const std::array<uint8, 16> key = dimensions::TagKey(tag.data());
			std::array<uint8, 8> plain{};
			dimensions::PutLE32(plain.data(), id);
			dimensions::PutLE32(plain.data() + 4, id);
			const std::array<uint8, 8> enc = dimensions::TeaEncrypt(plain.data(), key.data());
			memcpy(&tag[36 * 4], enc.data(), 8);
			const std::array<uint8, 4> pwd = dimensions::TagPassword(tag.data());
			memcpy(&tag[43 * 4], pwd.data(), 4);
		}
		else
		{
			// blank tag: the game fills it in; page 38 marks it as blank
			tag[38 * 4 + 1] = 1;
		}
		return out->writeData(tag.data(), (sint32)tag.size()) == (sint32)tag.size();
	}

	std::map<const uint32, const char*> DimensionsUSB::GetListMinifigs()
	{
		return ToMap(kMinifigNames);
	}

	std::map<const uint32, const char*> DimensionsUSB::GetListTokens()
	{
		return ToMap(kTokenNames);
	}

	std::string DimensionsUSB::FindFigure(uint32 figNum)
	{
		for (const auto& list : {std::span<const DimensionsName>(kMinifigNames), std::span<const DimensionsName>(kTokenNames)})
			for (const DimensionsName& e : list)
				if (e.id == figNum)
					return e.name;
		return fmt::format("Unknown ({})", figNum);
	}

	void DimensionsUSB::Tag::Flush()
	{
		if (!file)
			return;
		file->SetPosition(0);
		file->writeData(data.data(), (sint32)data.size());
	}
} // namespace nsyshid
