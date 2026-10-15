package main

// Archipelago requests for a Nintendo 64 game, whose memory is in this program.

import (
	"errors"
	"time"

	"retropixels/internal/apbridge"
)

type n64APReq struct {
	ops   []apOp
	reply chan []apRes
}

func runN64Ops(session string, p *apPage, ops []apOp) ([]apRes, error) {
	n64Mu.Lock()
	s := n64Sessions[session]
	n64Mu.Unlock()
	if s == nil {
		return nil, errNoGame
	}
	req := n64APReq{ops: ops, reply: make(chan []apRes, 1)}
	select {
	case s.cmd <- req:
	case <-s.done:
		return nil, errNoGame
	case <-time.After(apbridge.CallTimeout):
		return nil, errors.New("the Nintendo 64 emulator is busy")
	}
	var res []apRes
	select {
	case res = <-req.reply:
	case <-s.done:
		return nil, errNoGame
	case <-time.After(apbridge.CallTimeout):
		return nil, errors.New("the Nintendo 64 emulator didn't answer")
	}
	// On-screen messages are shown by the page.
	for i, o := range ops {
		if i < len(res) && o.T == "msg" && p != nil {
			_ = p.send(map[string]any{"ev": "msg", "text": o.Text})
		}
	}
	return res, nil
}

// apOps runs on the emulator goroutine, between frames. (The page holds the
// game still for LOCK; messages are shown by the page.)
func (s *n64Session) apOps(ops []apOp) []apRes {
	if s.m == nil {
		return []apRes{{Err: "the Nintendo 64 emulator has stopped"}}
	}
	return apbridge.Exec(s.m, ops, nil)
}
