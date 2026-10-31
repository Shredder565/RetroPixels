package main

// Streaming to YouTube (stream_windows.go): the stream key (kept encrypted
// for this Windows account), where the live chat is, and the quality.
type StreamSettings struct {
	Key     string `json:"key,omitempty"`     // the stream key, encrypted (Windows DPAPI), base64
	Chat    string `json:"chat,omitempty"`    // the channel (@name) or the stream's link
	Quality string `json:"quality,omitempty"` // stream.Qualities ID
	NoChat  bool   `json:"noAutoChat,omitempty"`
}
