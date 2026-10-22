//go:build !windows

package ffmpeg

import "os/exec"

func hide(cmd *exec.Cmd) {}

// LowPriority runs ffmpeg below normal priority, so encoding never slows the
// game: through nice -n 10 (Go can't set a child's priority itself). The
// process ID stays ffmpeg's own, since nice becomes the program it runs.
// Without nice the command is left as it is.
func LowPriority(cmd *exec.Cmd) *exec.Cmd {
	if cmd == nil || cmd.Process != nil || cmd.Err != nil || len(cmd.Args) == 0 {
		return cmd
	}
	nice, err := exec.LookPath("nice")
	if err != nil {
		return cmd
	}
	args := append([]string{"nice", "-n", "10", "--", cmd.Path}, cmd.Args[1:]...)
	cmd.Path, cmd.Args = nice, args
	return cmd
}
