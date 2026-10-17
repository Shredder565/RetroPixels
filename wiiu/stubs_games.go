package wiiu

// Library calls games make that need no work here: they set up things
// RetroPixels doesn't have (the error viewer, the screen capture rules) and report success.

func init() {
	zero := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 0) }
	one := func(m *Machine, cpu *CPU, t *Thread) { ret(cpu, 1) }
	for lib, names := range map[string][]string{
		"coreinit": {"OSSetScreenCapturePermission", "OSSetScreenCapturePermissionEx"},
		"snd_core": {"AXSetMode", "AXSetDRCVSMode", "AXSetDeviceMode"},
		"sndcore2": {"AXSetMode", "AXSetDRCVSMode", "AXSetDeviceMode"},
		"erreula": {"ErrEulaDestroy__3RplFv", "ErrEulaCalc__3RplFRCQ3_2nn7erreula14ControllerInfo",
			"ErrEulaDrawTV__3RplFv", "ErrEulaDrawDRC__3RplFv", "ErrEulaGetStateErrorViewer__3RplFv",
			"ErrEulaIsAppearHomeNixSign__3RplFv", "ErrEulaIsDecideSelectButtonError__3RplFv",
			"ErrEulaIsDecideSelectLeftButtonError__3RplFv", "ErrEulaIsDecideSelectRightButtonError__3RplFv",
			"ErrEulaAppearHomeNixSign__3RplFRCQ3_2nn7erreula14HomeNixSignArg", "ErrEulaDisappearHomeNixSign__3RplFv",
			"ErrEulaAppearError__3RplFRCQ3_2nn7erreula9AppearArg", "ErrEulaDisappearError__3RplFv",
			"ErrEulaChangeLang__3RplFQ3_2nn7erreula8LangType", "ErrEulaPlayAppearSE__3RplFb",
			"ErrEulaSetControllerRemo__3RplFQ3_2nn7erreula14ControllerType", "ErrEulaGetResultCode__3RplFv",
			"ErrEulaGetResultType__3RplFv", "ErrEulaSetVersion__3RplFi", "ErrEulaIsSelectCursorActive__3RplFv"},
	} {
		for _, n := range names {
			if hleFuncs[lib][n] == nil {
				reg(lib, n, zero)
			}
		}
	}
	reg("erreula", "ErrEulaCreate__3RplFPUcQ3_2nn7erreula10RegionTypeQ3_2nn7erreula8LangTypeP8FSClient", one)
}
