package ps3

import "encoding/hex"

// Keys used by the PS3 to protect PSN games. They are facts about the
// console, published by the PS3 homebrew community; only the ones the
// Turtles in Time package needs are here.

func mustHex(s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		panic(err)
	}
	return b
}

var (
	klicFree  = mustHex("72F990788F9CFF745725F08E4C128387") // free-licence games' klicensee
	klicKey   = mustHex("F2FBCA7A75B04EDC1390638CCDFDD1EE") // turns a klicensee into the SELF's outer key
	omacKey2  = mustHex("6BA52976EFDA16EF3C339FB2971E256B")
	edatKey0  = mustHex("BE959CA8308DEFA2E5E180C63712A9AE")
	edatHash0 = mustHex("EFFE5BD1652EEBC11918CF7C04D4F011")
	edatKey1  = mustHex("4CA9C14B01C95309969BEC68AA0BC081")
	edatHash1 = mustHex("3D92699B705B073854D8FCC6C7672747")
	sdatKey   = mustHex("0D655EF8E674A98AB8505CFA7D012933")
)

// npdrmKey is one key set for PSN (NPDRM) programs, chosen by the SELF's key revision.
type npdrmKey struct{ erk, riv []byte }

var npdrmKeys = map[uint16]npdrmKey{
	0x0001: {mustHex("F9EDD0301F770FABBA8863D9897F0FEA6551B09431F61312654E28F43533EA6B"), mustHex("A551CCB4A42C37A734A2B4F9657D5540")},
	0x0002: {mustHex("8E737230C80E66AD0162EDDD32F1F774EE5E4E187449F19079437A508FCF9C86"), mustHex("7AAECC60AD12AED90C348D8C11D2BED5")},
}
