// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// Disney Infinity base protocol: the game writes 32-byte commands
// (0xFF, length, command, sequence, payload...) and reads 32-byte replies
// (0xAA, length, sequence, payload..., checksum). Figure placements and
// removals are announced with unsolicited 0xAB reports. Every reply ends in
// an 8-bit sum of the preceding bytes.
//
// Figures are 20 blocks of 16 bytes; block 1 holds the figure number,
// AES-128 encrypted with a key derived (SHA-1) from the tag's 7-byte UID.
#include "Infinity.h"

#include <random>

#include "util/crypto/aes128.h"
#include <openssl/evp.h>

namespace nsyshid
{
	InfinityUSB g_infinitybase;

	// ---- protocol arithmetic
	namespace infinity
	{
		// bits of a 64-bit challenge word that carry data; the others are filler
		constexpr uint64 SCRAMBLE_MASK = 0x8E55AA1B3999E8AAull;

		uint8 Checksum(const uint8* data, size_t count)
		{
			uint32 sum = 0;
			for (size_t i = 0; i < count; i++)
				sum += data[i];
			return (uint8)sum;
		}

		// data bits sit where SCRAMBLE_MASK has ones; the lowest data bit is the
		// one closest to the word's most significant end
		uint64 Scramble(uint32 value, uint32 filler)
		{
			uint64 out = 0;
			for (int i = 0; i < 64; i++)
			{
				uint64 bit;
				if ((SCRAMBLE_MASK >> i) & 1)
				{
					bit = value & 1;
					value >>= 1;
				}
				else
				{
					bit = filler & 1;
					filler >>= 1;
				}
				out |= bit << (63 - i);
			}
			return out;
		}

		uint32 Descramble(uint64 word)
		{
			uint32 out = 0;
			for (int i = 0; i < 64; i++)
				if ((SCRAMBLE_MASK >> (63 - i)) & 1)
					out = (out << 1) | (uint32)((word >> i) & 1);
			return out;
		}

		// Jenkins small fast generator (rotations 27 and 17)
		uint32 RngNext(uint32& a, uint32& b, uint32& c, uint32& d)
		{
			const uint32 e = a - std::rotl(b, 27);
			a = b ^ std::rotl(c, 17);
			b = c + d;
			c = d + e;
			d = e + a;
			return d;
		}
		void RngSeed(uint32& a, uint32& b, uint32& c, uint32& d, uint32 seed)
		{
			a = 0xF1EA5EED;
			b = c = d = seed;
			for (int i = 0; i < 23; i++)
				RngNext(a, b, c, d);
		}

		// CRC-32 (reflected, polynomial 0xEDB88320) without pre/post inversion
		uint32 Crc32(const uint8* data, size_t count)
		{
			uint32 crc = 0;
			for (size_t i = 0; i < count; i++)
			{
				crc ^= data[i];
				for (int bit = 0; bit < 8; bit++)
					crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
			}
			return crc;
		}

		// key = SHA-1(16 fixed bytes, "(c) Disney 2013", UID), first 16 bytes,
		// each 32-bit word byte-reversed
		std::array<uint8, 16> FigureKey(const uint8* uid7)
		{
			static constexpr uint8 kPrefix[31] = {
				0xAF, 0x62, 0xD2, 0xEC, 0x04, 0x91, 0x96, 0x8C, 0xC5, 0x2A, 0x1A, 0x71, 0x65, 0xF8, 0x65, 0xFE,
				0x28, 0x63, 0x29, 0x20, 0x44, 0x69, 0x73, 0x6E, 0x65, 0x79, 0x20, 0x32, 0x30, 0x31, 0x33};
			uint8 message[sizeof(kPrefix) + 7];
			memcpy(message, kPrefix, sizeof(kPrefix));
			memcpy(message + sizeof(kPrefix), uid7, 7);
			uint8 digest[EVP_MAX_MD_SIZE];
			unsigned int digestLength = 0;
			EVP_Digest(message, sizeof(message), digest, &digestLength, EVP_sha1(), nullptr);
			std::array<uint8, 16> key{};
			for (int w = 0; w < 4; w++)
				for (int i = 0; i < 4; i++)
					key[w * 4 + i] = digest[w * 4 + (3 - i)];
			return key;
		}

		// plain contents of block 1 of a new figure
		std::array<uint8, 16> NewFigureBlock(uint32 number, uint8 series)
		{
			std::array<uint8, 16> block{};
			block[1] = (uint8)(number >> 16);
			block[2] = (uint8)(number >> 8);
			block[3] = (uint8)number;
			// manufacturing date (YY MM DD) = release of the figure's series
			static constexpr uint8 kDates[3][3] = {{13, 8, 18}, {14, 9, 18}, {15, 8, 28}};
			if (series >= 1 && series <= 3)
				memcpy(&block[4], kDates[series - 1], 3);
			block[9] = 0x01;
			block[10] = 0xD1;
			block[11] = 0x1F;
			const uint32 crc = Crc32(block.data(), 12);
			for (int i = 0; i < 4; i++)
				block[12 + i] = (uint8)(crc >> (24 - 8 * i));
			return block;
		}
	}
	// ---- device

	namespace
	{
		struct InfinityName
		{
			uint32 number;
			uint8 series;
			const char* name;
		};

		constexpr InfinityName kInfinityNames[] = {
		{0x0F4241, 1, "Mr. Incredible"},
		{0x0F4242, 1, "Sulley"},
		{0x0F4243, 1, "Jack Sparrow"},
		{0x0F4244, 1, "Lone Ranger"},
		{0x0F4245, 1, "Tonto"},
		{0x0F4246, 1, "Lightning McQueen"},
		{0x0F4247, 1, "Holley Shiftwell"},
		{0x0F4248, 1, "Buzz Lightyear"},
		{0x0F4249, 1, "Jessie"},
		{0x0F424A, 1, "Mike"},
		{0x0F424B, 1, "Mrs. Incredible"},
		{0x0F424C, 1, "Hector Barbossa"},
		{0x0F424D, 1, "Davy Jones"},
		{0x0F424E, 1, "Randy"},
		{0x0F424F, 1, "Syndrome"},
		{0x0F4250, 1, "Woody"},
		{0x0F4251, 1, "Mater"},
		{0x0F4252, 1, "Dash"},
		{0x0F4253, 1, "Violet"},
		{0x0F4254, 1, "Francesco Bernoulli"},
		{0x0F4255, 1, "Sorcerer's Apprentice Mickey"},
		{0x0F4256, 1, "Jack Skellington"},
		{0x0F4257, 1, "Rapunzel"},
		{0x0F4258, 1, "Anna"},
		{0x0F4259, 1, "Elsa"},
		{0x0F425A, 1, "Phineas"},
		{0x0F425B, 1, "Agent P"},
		{0x0F425C, 1, "Wreck-It Ralph"},
		{0x0F425D, 1, "Vanellope"},
		{0x0F425E, 1, "Mr. Incredible (Crystal)"},
		{0x0F425F, 1, "Jack Sparrow (Crystal)"},
		{0x0F4260, 1, "Sulley (Crystal)"},
		{0x0F4261, 1, "Lightning McQueen (Crystal)"},
		{0x0F4262, 1, "Lone Ranger (Crystal)"},
		{0x0F4263, 1, "Buzz Lightyear (Crystal)"},
		{0x0F4264, 1, "Agent P (Crystal)"},
		{0x0F4265, 1, "Sorcerer's Apprentice Mickey (Crystal)"},
		{0x0F4266, 1, "Buzz Lightyear (Glowing)"},
		{0x0F42A4, 2, "Captain America"},
		{0x0F42A5, 2, "Hulk"},
		{0x0F42A6, 2, "Iron Man"},
		{0x0F42A7, 2, "Thor"},
		{0x0F42A8, 2, "Groot"},
		{0x0F42A9, 2, "Rocket Raccoon"},
		{0x0F42AA, 2, "Star-Lord"},
		{0x0F42AB, 2, "Spider-Man"},
		{0x0F42AC, 2, "Nick Fury"},
		{0x0F42AD, 2, "Black Widow"},
		{0x0F42AE, 2, "Hawkeye"},
		{0x0F42AF, 2, "Drax"},
		{0x0F42B0, 2, "Gamora"},
		{0x0F42B1, 2, "Iron Fist"},
		{0x0F42B2, 2, "Nova"},
		{0x0F42B3, 2, "Venom"},
		{0x0F42B4, 2, "Donald Duck"},
		{0x0F42B5, 2, "Aladdin"},
		{0x0F42B6, 2, "Stitch"},
		{0x0F42B7, 2, "Merida"},
		{0x0F42B8, 2, "Tinker Bell"},
		{0x0F42B9, 2, "Maleficent"},
		{0x0F42BA, 2, "Hiro"},
		{0x0F42BB, 2, "Baymax"},
		{0x0F42BC, 2, "Loki"},
		{0x0F42BD, 2, "Ronan"},
		{0x0F42BE, 2, "Green Goblin"},
		{0x0F42BF, 2, "Falcon"},
		{0x0F42C0, 2, "Yondu"},
		{0x0F42C1, 2, "Jasmine"},
		{0x0F42C6, 2, "Black Suit Spider-Man"},
		{0x0F42D6, 3, "Sam Flynn"},
		{0x0F42D7, 3, "Quorra"},
		{0x0F4308, 3, "Anakin Skywalker"},
		{0x0F4309, 3, "Obi-Wan Kenobi"},
		{0x0F430A, 3, "Yoda"},
		{0x0F430B, 3, "Ahsoka Tano"},
		{0x0F430C, 3, "Darth Maul"},
		{0x0F430E, 3, "Luke Skywalker"},
		{0x0F430F, 3, "Han Solo"},
		{0x0F4310, 3, "Princess Leia"},
		{0x0F4311, 3, "Chewbacca"},
		{0x0F4312, 3, "Darth Vader"},
		{0x0F4313, 3, "Boba Fett"},
		{0x0F4314, 3, "Ezra Bridger"},
		{0x0F4315, 3, "Kanan Jarrus"},
		{0x0F4316, 3, "Sabine Wren"},
		{0x0F4317, 3, "Zeb Orrelios"},
		{0x0F4318, 3, "Joy"},
		{0x0F4319, 3, "Anger"},
		{0x0F431A, 3, "Fear"},
		{0x0F431B, 3, "Sadness"},
		{0x0F431C, 3, "Disgust"},
		{0x0F431D, 3, "Mickey Mouse"},
		{0x0F431E, 3, "Minnie Mouse"},
		{0x0F431F, 3, "Mulan"},
		{0x0F4320, 3, "Olaf"},
		{0x0F4321, 3, "Vision"},
		{0x0F4322, 3, "Ultron"},
		{0x0F4323, 3, "Ant-Man"},
		{0x0F4325, 3, "Captain America - The First Avenger"},
		{0x0F4326, 3, "Finn"},
		{0x0F4327, 3, "Kylo Ren"},
		{0x0F4328, 3, "Poe Dameron"},
		{0x0F4329, 3, "Rey"},
		{0x0F432B, 3, "Spot"},
		{0x0F432C, 3, "Nick Wilde"},
		{0x0F432D, 3, "Judy Hopps"},
		{0x0F432E, 3, "Hulkbuster"},
		{0x0F432F, 3, "Anakin Skywalker (Light FX)"},
		{0x0F4330, 3, "Obi-Wan Kenobi (Light FX)"},
		{0x0F4331, 3, "Yoda (Light FX)"},
		{0x0F4332, 3, "Luke Skywalker (Light FX)"},
		{0x0F4333, 3, "Darth Vader (Light FX)"},
		{0x0F4334, 3, "Kanan Jarrus (Light FX)"},
		{0x0F4335, 3, "Kylo Ren (Light FX)"},
		{0x0F4336, 3, "Black Panther"},
		{0x0F436C, 3, "Nemo"},
		{0x0F436D, 3, "Dory"},
		{0x0F436E, 3, "Baloo"},
		{0x0F436F, 3, "Alice"},
		{0x0F4370, 3, "Mad Hatter"},
		{0x0F4371, 3, "Time"},
		{0x0F4372, 3, "Peter Pan"},
		{0x1E8481, 1, "Starter Play Set"},
		{0x1E8482, 1, "Lone Ranger Play Set"},
		{0x1E8483, 1, "Cars Play Set"},
		{0x1E8484, 1, "Toy Story in Space Play Set"},
		{0x1E84E4, 2, "Marvel's The Avengers Play Set"},
		{0x1E84E5, 2, "Marvel's Spider-Man Play Set"},
		{0x1E84E6, 2, "Marvel's Guardians of the Galaxy Play Set"},
		{0x1E84E7, 2, "Assault on Asgard"},
		{0x1E84E8, 2, "Escape from the Kyln"},
		{0x1E84E9, 2, "Stitch's Tropical Rescue"},
		{0x1E84EA, 2, "Brave Forest Siege"},
		{0x1E8548, 3, "Inside Out Play Set"},
		{0x1E8549, 3, "Star Wars: Twilight of the Republic Play Set"},
		{0x1E854A, 3, "Star Wars: Rise Against the Empire Play Set"},
		{0x1E854B, 3, "Star Wars: The Force Awakens Play Set"},
		{0x1E854C, 3, "Marvel Battlegrounds Play Set"},
		{0x1E854D, 3, "Toy Box Speedway"},
		{0x1E854E, 3, "Toy Box Takeover"},
		{0x1E85AC, 3, "Finding Dory Play Set"},
		{0x2DC6C3, 1, "Bolt's Super Strength"},
		{0x2DC6C4, 1, "Ralph's Power of Destruction"},
		{0x2DC6C5, 1, "Chernabog's Power"},
		{0x2DC6C6, 1, "C.H.R.O.M.E. Damage Increaser"},
		{0x2DC6C7, 1, "Dr. Doofenshmirtz's Damage-Inator!"},
		{0x2DC6C8, 1, "Electro-Charge"},
		{0x2DC6C9, 1, "Fix-It Felix's Repair Power"},
		{0x2DC6CA, 1, "Rapunzel's Healing"},
		{0x2DC6CB, 1, "C.H.R.O.M.E. Armor Shield"},
		{0x2DC6CC, 1, "Star Command Shield"},
		{0x2DC6CD, 1, "Violet's Force Field"},
		{0x2DC6CE, 1, "Pieces of Eight"},
		{0x2DC6CF, 1, "Scrooge McDuck's Lucky Dime"},
		{0x2DC6D0, 1, "User Control"},
		{0x2DC6D1, 1, "Sorcerer Mickey's Hat"},
		{0x2DC6FE, 1, "Emperor Zurg's Wrath"},
		{0x2DC6FF, 1, "Merlin's Summon"},
		{0x2DC765, 2, "Enchanted Rose"},
		{0x2DC766, 2, "Mulan's Training Uniform"},
		{0x2DC767, 2, "Flubber"},
		{0x2DC768, 2, "S.H.I.E.L.D. Helicarrier Strike"},
		{0x2DC769, 2, "Zeus' Thunderbolts"},
		{0x2DC76A, 2, "King Louie's Monkeys"},
		{0x2DC76B, 2, "Infinity Gauntlet"},
		{0x2DC76D, 2, "Sorcerer Supreme"},
		{0x2DC76E, 2, "Maleficent's Spell Cast"},
		{0x2DC76F, 2, "Chernabog's Spirit Cyclone"},
		{0x2DC770, 2, "Marvel Team-Up: Capt. Marvel"},
		{0x2DC771, 2, "Marvel Team-Up: Iron Patriot"},
		{0x2DC772, 2, "Marvel Team-Up: Ant-Man"},
		{0x2DC773, 2, "Marvel Team-Up: White Tiger"},
		{0x2DC774, 2, "Marvel Team-Up: Yondu"},
		{0x2DC775, 2, "Marvel Team-Up: Winter Soldier"},
		{0x2DC776, 2, "Stark Arc Reactor"},
		{0x2DC777, 2, "Gamma Rays"},
		{0x2DC778, 2, "Alien Symbiote"},
		{0x2DC779, 2, "All for One"},
		{0x2DC77A, 2, "Sandy Claws Surprise"},
		{0x2DC77B, 2, "Glory Days"},
		{0x2DC77C, 2, "Cursed Pirate Gold"},
		{0x2DC77D, 2, "Sentinel of Liberty"},
		{0x2DC77E, 2, "The Immortal Iron Fist"},
		{0x2DC77F, 2, "Space Armor"},
		{0x2DC780, 2, "Rags to Riches"},
		{0x2DC781, 2, "Ultimate Falcon"},
		{0x2DC788, 3, "Tomorrowland Time Bomb"},
		{0x2DC78E, 3, "Galactic Team-Up: Mace Windu"},
		{0x2DC791, 3, "Luke's Rebel Alliance Flight Suit Costume"},
		{0x2DC798, 3, "Finn's Stormtrooper Costume"},
		{0x2DC799, 3, "Poe's Resistance Jacket"},
		{0x2DC79A, 3, "Resistance Tactical Strike"},
		{0x2DC79E, 3, "Officer Nick Wilde"},
		{0x2DC79F, 3, "Meter Maid Judy"},
		{0x2DC7A2, 3, "Darkhawk's Blast"},
		{0x2DC7A3, 3, "Cosmic Cube Blast"},
		{0x2DC7A4, 3, "Princess Leia's Boushh Disguise"},
		{0x2DC7A6, 3, "Nova Corps Strike"},
		{0x2DC7A7, 3, "King Mickey"},
		{0x3D0912, 1, "Mickey's Car"},
		{0x3D0913, 1, "Cinderella's Coach"},
		{0x3D0914, 1, "Electric Mayhem Bus"},
		{0x3D0915, 1, "Cruella De Vil's Car"},
		{0x3D0916, 1, "Pizza Planet Delivery Truck"},
		{0x3D0917, 1, "Mike's New Car"},
		{0x3D0919, 1, "Parking Lot Tram"},
		{0x3D091A, 1, "Captain Hook's Ship"},
		{0x3D091B, 1, "Dumbo"},
		{0x3D091C, 1, "Calico Helicopter"},
		{0x3D091D, 1, "Maximus"},
		{0x3D091E, 1, "Angus"},
		{0x3D091F, 1, "Abu the Elephant"},
		{0x3D0920, 1, "Headless Horseman's Horse"},
		{0x3D0921, 1, "Phillipe"},
		{0x3D0922, 1, "Khan"},
		{0x3D0923, 1, "Tantor"},
		{0x3D0924, 1, "Dragon Firework Cannon"},
		{0x3D0925, 1, "Stitch's Blaster"},
		{0x3D0926, 1, "Toy Story Mania Blaster"},
		{0x3D0927, 1, "Flamingo Croquet Mallet"},
		{0x3D0928, 1, "Carl Fredricksen's Cane"},
		{0x3D0929, 1, "Hangin' Ten Stitch With Surfboard"},
		{0x3D092A, 1, "Condorman Glider"},
		{0x3D092B, 1, "WALL-E's Fire Extinguisher"},
		{0x3D092C, 1, "On the Grid"},
		{0x3D092D, 1, "WALL-E's Collection"},
		{0x3D092E, 1, "King Candy's Dessert Toppings"},
		{0x3D0930, 1, "Victor's Experiments"},
		{0x3D0931, 1, "Jack's Scary Decorations"},
		{0x3D0933, 1, "Frozen Flourish"},
		{0x3D0934, 1, "Rapunzel's Kingdom"},
		{0x3D0935, 1, "TRON Interface"},
		{0x3D0936, 1, "Buy N Large Atmosphere"},
		{0x3D0937, 1, "Sugar Rush Sky"},
		{0x3D0939, 1, "New Holland Skyline"},
		{0x3D093A, 1, "Halloween Town Sky"},
		{0x3D093C, 1, "Chill in the Air"},
		{0x3D093D, 1, "Rapunzel's Birthday Sky"},
		{0x3D0940, 1, "Astro Blasters Space Cruiser"},
		{0x3D0941, 1, "Marlin's Reef"},
		{0x3D0942, 1, "Nemo's Seascape"},
		{0x3D0943, 1, "Alice's Wonderland"},
		{0x3D0944, 1, "Tulgey Wood"},
		{0x3D0945, 1, "Tri-State Area Terrain"},
		{0x3D0946, 1, "Danville Sky"},
		{0x3D0965, 2, "Stark Tech"},
		{0x3D0966, 2, "Spider-Streets"},
		{0x3D0967, 2, "World War Hulk"},
		{0x3D0968, 2, "Gravity Falls Forest"},
		{0x3D0969, 2, "Neverland"},
		{0x3D096A, 2, "Simba's Pridelands"},
		{0x3D096C, 2, "Calhoun's Command"},
		{0x3D096D, 2, "Star-Lord's Galaxy"},
		{0x3D096E, 2, "Dinosaur World"},
		{0x3D096F, 2, "Groot's Roots"},
		{0x3D0970, 2, "Mulan's Countryside"},
		{0x3D0971, 2, "The Sands of Agrabah"},
		{0x3D0974, 2, "A Small World"},
		{0x3D0975, 2, "View from the Suit"},
		{0x3D0976, 2, "Spider-Sky"},
		{0x3D0977, 2, "World War Hulk Sky"},
		{0x3D0978, 2, "Gravity Falls Sky"},
		{0x3D0979, 2, "Second Star to the Right"},
		{0x3D097A, 2, "The King's Domain"},
		{0x3D097C, 2, "CyBug Swarm"},
		{0x3D097D, 2, "The Rip"},
		{0x3D097E, 2, "Forgotten Skies"},
		{0x3D097F, 2, "Groot's View"},
		{0x3D0980, 2, "The Middle Kingdom"},
		{0x3D0984, 2, "Skies of the World"},
		{0x3D0985, 2, "S.H.I.E.L.D. Containment Truck"},
		{0x3D0986, 2, "Main Street Electrical Parade Float"},
		{0x3D0987, 2, "Mr. Toad's Motorcar"},
		{0x3D0988, 2, "Le Maximum"},
		{0x3D0989, 2, "Alice in Wonderland's Caterpillar"},
		{0x3D098A, 2, "Eglantine's Motorcycle"},
		{0x3D098B, 2, "Medusa's Swamp Mobile"},
		{0x3D098C, 2, "Hydra Motorcycle"},
		{0x3D098D, 2, "Darkwing Duck's Ratcatcher"},
		{0x3D098F, 2, "The USS Swinetrek"},
		{0x3D0991, 2, "Spider-Copter"},
		{0x3D0992, 2, "Aerial Area Rug"},
		{0x3D0993, 2, "Jack-O-Lantern's Glider"},
		{0x3D0994, 2, "Spider-Buggy"},
		{0x3D0995, 2, "Jack Skellington's Reindeer"},
		{0x3D0996, 2, "Fantasyland Carousel Horse"},
		{0x3D0997, 2, "Odin's Horse"},
		{0x3D0998, 2, "Gus the Mule"},
		{0x3D099A, 2, "Darkwing Duck's Grappling Gun"},
		{0x3D099C, 2, "Ghost Rider's Chain Whip"},
		{0x3D099D, 2, "Lew Zealand's Boomerang Fish"},
		{0x3D099E, 2, "Sergeant Calhoun's Blaster"},
		{0x3D09A0, 2, "Falcon's Wings"},
		{0x3D09A1, 2, "Mabel's Kittens for Fists"},
		{0x3D09A2, 2, "Jim Hawkins' Solar Board"},
		{0x3D09A3, 2, "Black Panther's Vibranium Knives"},
		{0x3D09A4, 2, "Cloak of Levitation"},
		{0x3D09A5, 2, "Aladdin's Magic Carpet"},
		{0x3D09A6, 2, "Honey Lemon's Ice Capsules"},
		{0x3D09A7, 2, "Jasmine's Palace View"},
		{0x3D09C1, 2, "Lola"},
		{0x3D09C2, 2, "Spider-Cycle"},
		{0x3D09C3, 2, "The Avenjet"},
		{0x3D09C4, 2, "Spider-Glider"},
		{0x3D09C5, 2, "Light Cycle"},
		{0x3D09C6, 2, "Light Jet"},
		{0x3D09C9, 3, "Retro Ray Gun"},
		{0x3D09CA, 3, "Tomorrowland Futurescape"},
		{0x3D09CB, 3, "Tomorrowland Stratosphere"},
		{0x3D09CC, 3, "Skies Over Felucia"},
		{0x3D09CD, 3, "Forests of Felucia"},
		{0x3D09CF, 3, "General Grievous' Wheel Bike"},
		{0x3D09D2, 3, "Slave I Flyer"},
		{0x3D09D3, 3, "Y-Wing Fighter"},
		{0x3D09D4, 3, "Arlo"},
		{0x3D09D5, 3, "Nash"},
		{0x3D09D6, 3, "Butch"},
		{0x3D09D7, 3, "Ramsey"},
		{0x3D09DC, 3, "Stars Over Sahara Square"},
		{0x3D09DD, 3, "Sahara Square Sands"},
		{0x3D09E0, 3, "Ghost Rider's Motorcycle"},
		{0x3D09E5, 3, "Quad Jumper"},
		};

		// which part of the base a UI position belongs to (1 hexagon, 2/3 players)
		uint8 BasePart(uint8 position)
		{
			return position < MAX_FIGURES ? (uint8)(position / 3 + 1) : 0;
		}

		void SleepMs(int ms)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(ms));
		}
	}

	InfinityBaseDevice::InfinityBaseDevice()
		: EmulatedHidDevice(0x0E6F, 0x0129)
	{
	}

	Device::Status InfinityBaseDevice::Read(uint8* buffer, uint32 length, sint32& bytesRead)
	{
		InfinityUSB::Report r = g_infinitybase.GetStatus();
		memcpy(buffer, r.data(), std::min<size_t>(length, r.size()));
		bytesRead = (sint32)length;
		SleepMs(1);
		return Status::Ok;
	}

	Device::Status InfinityBaseDevice::Write(uint8* data, uint32 length, sint32& bytesWritten)
	{
		g_infinitybase.SendCommand(data, length);
		bytesWritten = (sint32)length;
		return Status::Ok;
	}

	// ---- base state ---------------------------------------------------------------

	void InfinityUSB::Rng::Seed(uint32 seed)
	{
		infinity::RngSeed(a, b, c, d, seed);
	}

	uint32 InfinityUSB::Rng::Next()
	{
		return infinity::RngNext(a, b, c, d);
	}

	// blocks until a figure event or a reply is available (figure events first)
	InfinityUSB::Report InfinityUSB::GetStatus()
	{
		while (true)
		{
			{
				std::unique_lock _l(m_lock);
				std::queue<Report>* source = !m_figureEvents.empty() ? &m_figureEvents : !m_replies.empty() ? &m_replies : nullptr;
				if (source)
				{
					Report r = source->front();
					source->pop();
					return r;
				}
			}
			SleepMs(100);
		}
	}

	void InfinityUSB::SendCommand(uint8* buf, uint32 length)
	{
		Report cmd{};
		memcpy(cmd.data(), buf, std::min<size_t>(length, cmd.size()));
		const uint8 command = cmd[2];
		const uint8 seq = cmd[3];

		auto ack = [seq](Report& r) {
			r[0] = 0xAA;
			r[1] = 0x01;
			r[2] = seq;
			r[3] = infinity::Checksum(r.data(), 3);
		};

		std::unique_lock _l(m_lock);
		Report reply{};
		switch (command)
		{
		case 0x80: // activate: fixed identification reply
			reply = {0xAA, 0x15, 0x00, 0x00, 0x0F, 0x01, 0x00, 0x03, 0x02, 0x09, 0x09, 0x43,
					 0x20, 0x32, 0x62, 0x36, 0x36, 0x4B, 0x34, 0x99, 0x67, 0x31, 0x93, 0x8C};
			break;
		case 0x81: // challenge seed (big-endian scrambled word)
		{
			uint64 word = 0;
			for (int i = 0; i < 8; i++)
				word = (word << 8) | cmd[4 + i];
			m_rng.Seed(infinity::Descramble(word));
			ack(reply);
			break;
		}
		case 0x83: // challenge answer: next random number, scrambled
		{
			const uint64 word = infinity::Scramble(m_rng.Next(), 0);
			reply[0] = 0xAA;
			reply[1] = 0x09;
			reply[2] = seq;
			for (int i = 0; i < 8; i++)
				reply[3 + i] = (uint8)(word >> (56 - 8 * i));
			reply[11] = infinity::Checksum(reply.data(), 11);
			break;
		}
		case 0x90: // light commands
		case 0x92:
		case 0x93:
		case 0x95:
		case 0x96:
		case 0xB5: // status
			ack(reply);
			break;
		case 0xA1:
			ListPresent(seq, reply);
			break;
		case 0xA2:
			ReadBlock(cmd[4], cmd[5], seq, reply);
			break;
		case 0xA3:
			WriteBlock(cmd[4], cmd[5], &cmd[7], seq, reply);
			break;
		case 0xB4:
			ReadIdentifier(cmd[4], seq, reply);
			break;
		default:
			cemuLog_log(LogType::Force, "Infinity base: unknown command 0x{:02x}", command);
			break;
		}
		m_replies.push(reply);
	}

	void InfinityUSB::ListPresent(uint8 seq, Report& reply)
	{
		size_t at = 3;
		for (uint8 i = 0; i < MAX_FIGURES; i++)
		{
			if (!m_figures[i].present)
				continue;
			const uint8 group = i == 0 ? 0x10 : i < 4 ? 0x20 : 0x30;
			reply[at] = group + m_figures[i].order;
			reply[at + 1] = 0x09;
			at += 2;
		}
		reply[0] = 0xAA;
		reply[1] = (uint8)(at - 2);
		reply[2] = seq;
		reply[at] = infinity::Checksum(reply.data(), at);
	}

	InfinityUSB::Figure& InfinityUSB::FigureByOrder(uint8 order)
	{
		for (Figure& f : m_figures)
			if (f.order == order)
				return f;
		return m_figures[0];
	}

	// the game numbers blocks 0, 1, 2, ...; they map to file blocks 1, 4, 8, ...
	static uint8 FileBlock(uint8 block)
	{
		return block == 0 ? 1 : (uint8)std::min(block * 4, 0xFF);
	}

	void InfinityUSB::ReadBlock(uint8 order, uint8 block, uint8 seq, Report& reply)
	{
		const Figure& f = FigureByOrder(order);
		reply[0] = 0xAA;
		reply[1] = 0x12;
		reply[2] = seq;
		reply[3] = 0x00;
		const uint8 fb = FileBlock(block);
		if (f.present && fb < INF_BLOCK_COUNT)
			memcpy(&reply[4], f.data.data() + fb * INF_BLOCK_SIZE, INF_BLOCK_SIZE);
		reply[20] = infinity::Checksum(reply.data(), 20);
	}

	void InfinityUSB::WriteBlock(uint8 order, uint8 block, const uint8* data, uint8 seq, Report& reply)
	{
		Figure& f = FigureByOrder(order);
		reply[0] = 0xAA;
		reply[1] = 0x02;
		reply[2] = seq;
		reply[3] = 0x00;
		const uint8 fb = FileBlock(block);
		if (f.present && fb < INF_BLOCK_COUNT)
		{
			memcpy(f.data.data() + fb * INF_BLOCK_SIZE, data, INF_BLOCK_SIZE);
			f.Flush();
		}
		reply[4] = infinity::Checksum(reply.data(), 4);
	}

	void InfinityUSB::ReadIdentifier(uint8 order, uint8 seq, Report& reply)
	{
		const Figure& f = FigureByOrder(order);
		reply[0] = 0xAA;
		reply[1] = 0x09;
		reply[2] = seq;
		reply[3] = 0x00;
		if (f.present)
			memcpy(&reply[4], f.data.data(), 7);
		reply[11] = infinity::Checksum(reply.data(), 11);
	}

	void InfinityUSB::QueueFigureEvent(uint8 part, uint8 order, bool removed)
	{
		Report r{0xAB, 0x04, part, 0x09, order, (uint8)(removed ? 1 : 0)};
		r[6] = infinity::Checksum(r.data(), 6);
		m_figureEvents.push(r);
	}

	uint32 InfinityUSB::LoadFigure(const std::array<uint8, INF_FIGURE_SIZE>& buf, std::unique_ptr<FileStream> file, uint8 position)
	{
		std::unique_lock _l(m_lock);
		const uint8 part = BasePart(position);
		if (part == 0)
			return 0;

		// the figure number is in the encrypted block 1
		std::array<uint8, 16> key = infinity::FigureKey(buf.data());
		std::array<uint8, 16> cipher{};
		std::array<uint8, 16> plain{};
		memcpy(cipher.data(), &buf[16], 16);
		AES128_ECB_decrypt(cipher.data(), key.data(), plain.data());
		const uint32 number = ((uint32)plain[1] << 16) | ((uint32)plain[2] << 8) | plain[3];

		Figure& f = m_figures[position];
		f.file = std::move(file);
		f.data = buf;
		f.present = true;
		if (f.order == 255)
			f.order = m_nextOrder++;
		QueueFigureEvent(part, f.order, false);
		return number;
	}

	bool InfinityUSB::RemoveFigure(uint8 position)
	{
		std::unique_lock _l(m_lock);
		if (position >= MAX_FIGURES)
			return false;
		Figure& f = m_figures[position];
		f.Flush();
		f.file.reset();
		if (!f.present)
			return false;
		f.present = false;
		QueueFigureEvent(BasePart(position), f.order, true);
		return true;
	}

	bool InfinityUSB::CreateFigure(fs::path pathName, uint32 figureNum, uint8 series)
	{
		const std::array<uint8, 16> block1 = infinity::NewFigureBlock(figureNum, series);
		if (block1[1] == 0x00)
			return false; // not a valid figure number

		std::unique_ptr<FileStream> out(FileStream::createFile2(pathName));
		if (!out)
			return false;

		std::array<uint8, INF_FIGURE_SIZE> dump{};
		// sector trailers
		static constexpr uint8 kFirstTrailer[3] = {0x17, 0x87, 0x8E};
		static constexpr uint8 kOtherTrailer[3] = {0x77, 0x87, 0x88};
		memcpy(&dump[0x36], kFirstTrailer, 3);
		for (size_t sector = 1; sector < 5; sector++)
			memcpy(&dump[sector * 0x40 + 0x36], kOtherTrailer, 3);

		// block 0: random 7-byte UID followed by the fixed manufacturer bytes
		std::array<uint8, 16> block0 = {0, 0, 0, 0, 0, 0, 0, 0x89, 0x44, 0x00, 0xC2};
		std::random_device rd;
		std::uniform_int_distribution<int> byte(0, 255);
		for (int i = 0; i < 7; i++)
			block0[i] = (uint8)byte(rd);

		std::array<uint8, 16> key = infinity::FigureKey(block0.data());
		std::array<uint8, 16> plain = block1;
		std::array<uint8, 16> zero{};
		std::array<uint8, 16> encrypted{};
		std::array<uint8, 16> encryptedZero{};
		AES128_ECB_encrypt(plain.data(), key.data(), encrypted.data());
		AES128_ECB_encrypt(zero.data(), key.data(), encryptedZero.data());

		memcpy(&dump[0], block0.data(), 16);
		memcpy(&dump[1 * 16], encrypted.data(), 16);
		for (size_t b : {4, 8, 12, 13})
			memcpy(&dump[b * 16], encryptedZero.data(), 16);

		out->writeData(dump.data(), (sint32)dump.size());
		return true;
	}

	std::map<const uint32, const std::pair<const uint8, const char*>> InfinityUSB::GetFigureList()
	{
		std::map<const uint32, const std::pair<const uint8, const char*>> list;
		for (const InfinityName& e : kInfinityNames)
			list.emplace(e.number, std::make_pair(e.series, e.name));
		return list;
	}

	std::pair<uint8, std::string> InfinityUSB::FindFigure(uint32 figNum)
	{
		for (const InfinityName& e : kInfinityNames)
			if (e.number == figNum)
				return {e.series, e.name};
		return {0, fmt::format("Unknown Figure ({})", figNum)};
	}

	void InfinityUSB::Figure::Flush()
	{
		if (!file)
			return;
		file->SetPosition(0);
		file->writeData(data.data(), (sint32)data.size());
	}
} // namespace nsyshid
