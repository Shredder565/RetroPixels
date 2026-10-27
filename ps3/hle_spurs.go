package ps3

// SPURS queues. SPURS tasks (programs the SPURS kernel runs on the SPUs)
// aren't run yet, so nothing is ever pushed to a queue: a pop gives back
// an entry of zeros (what the game takes as "no result") rather than
// leaving the caller's buffer as it was, which the game then reads as a
// pointer.

func init() {
	// (taskset, queue, buffer, size, depth, direction)
	reg("_cellSpursQueueInitialize", func(m *Machine, t *Thread) {
		if m.spursQueues == nil {
			m.spursQueues = map[uint32]uint32{}
		}
		m.spursQueues[t.arg(1)] = t.arg(3)
		t.ret(0)
	})
	// (queue, buffer, isPeek, isBlocking)
	reg("cellSpursQueuePopBody", func(m *Machine, t *Thread) {
		size, ok := m.spursQueues[t.arg(0)]
		if !ok || size == 0 || size > 1024 {
			size = 16
		}
		if buf := t.arg(1); buf != 0 {
			m.Mem.Fill(buf, 0, size)
		}
		t.ret(0)
	})
}
