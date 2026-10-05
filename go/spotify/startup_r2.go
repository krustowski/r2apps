//go:build r2 && !r2netcheck && !r2faultcheck

package main

func startupChecks() string { return "Go audio test ready; R loads Spotify playlists." }
