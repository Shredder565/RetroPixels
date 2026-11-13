package handheld

func exeNames(names ...string) []string {
	out := make([]string, len(names))
	for i, n := range names {
		out[i] = n + ".exe"
	}
	return out
}
