//go:build !windows

package wiiu

import "errors"

func newHostGPU(*Latte) (hwRenderer, error) {
	return nil, errors.New("there's no host GPU backend on this system yet")
}
