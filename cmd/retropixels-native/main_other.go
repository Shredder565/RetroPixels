//go:build !windows

package main

import (
	"fmt"
	"os"
)

func main() {
	fmt.Fprintln(os.Stderr, "RetroPixels "+version+" is a Windows program.")
	os.Exit(1)
}
