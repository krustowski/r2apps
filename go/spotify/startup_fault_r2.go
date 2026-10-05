//go:build r2 && r2faultcheck && !r2netcheck

package main

// Diagnostic-only build verifies that runtime failures reach the hosted window.
func startupChecks() string {
	diagnosticStage("intentional diagnostic panic")
	panic("Spotify diagnostic panic")
}
