package main

// The WebSocket server lives in internal/websocket (Archipelago's SNI Client
// reaches RetroPixels Native through it too); these are its names here.

import (
	"net/http"

	"retropixels/internal/websocket"
)

const (
	wsContinuation = websocket.OpContinuation
	wsText         = websocket.OpText
	wsBinary       = websocket.OpBinary
	wsClose        = websocket.OpClose
	wsPing         = websocket.OpPing
	wsPong         = websocket.OpPong
)

var errWSClosed = websocket.ErrClosed

type wsConn = websocket.Conn

func wsUpgrade(w http.ResponseWriter, r *http.Request, maxSize int) (*wsConn, error) {
	return websocket.Upgrade(w, r, maxSize)
}
