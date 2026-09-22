package main

import (
	"encoding/json"
	"fmt"
	"strconv"
	"time"

	"github.com/krustowski/rou2exOS-apps/go/r2net"
)

// Alerting: what happens to the results once the checks have run.
//
// Upstream has five channels.  Three of them survive the move --- the remote
// API, a webhook and Pushgateway --- because they are ordinary HTTP endpoints
// that can be plain.  Telegram and Discord do not: both are HTTPS-only public
// APIs, and there is no TLS on this machine and no honest way to fake one.  A
// dish on r2 that must reach Telegram should push to a webhook on a host that
// can, which is one line of relay and keeps the token off the floppy.

// Results is the payload the machine channels receive, in upstream's shape so
// that the same API endpoint accepts both.
type Results struct {
	Map map[string]bool `json:"dish_results"`
}

// jobName is the name of the Prometheus job used for dish results.
const jobName = "dish_results"

// machineNotifier is anything that takes the structured results.
type machineNotifier interface {
	send(results *Results, failedCount int) error
	name() string
}

type alerter struct {
	cfg   *Config
	stack *r2net.Stack
	log   *logger

	machine []machineNotifier
}

// newAlerter builds the set of channels the flags asked for.  A channel whose
// URL does not parse is reported and dropped, not fatal: the other channels
// still get the results, which is the point of having several.
func newAlerter(cfg *Config, stack *r2net.Stack, log *logger) *alerter {
	a := &alerter{cfg: cfg, stack: stack, log: log}

	if cfg.ApiURL != "" {
		if url, err := plainHTTPURL(cfg.ApiURL); err != nil {
			log.Error("error creating new remote API sender: ", err)
		} else {
			a.machine = append(a.machine, &apiSender{
				alerter:     a,
				url:         url,
				headerName:  cfg.ApiHeaderName,
				headerValue: cfg.ApiHeaderValue,
			})
		}
	}

	if cfg.WebhookURL != "" {
		if url, err := plainHTTPURL(cfg.WebhookURL); err != nil {
			log.Error("error creating new webhook sender: ", err)
		} else {
			a.machine = append(a.machine, &webhookSender{alerter: a, url: url})
		}
	}

	if cfg.PushgatewayURL != "" {
		if url, err := plainHTTPURL(cfg.PushgatewayURL); err != nil {
			log.Error("error creating new Pushgateway sender: ", err)
		} else {
			a.machine = append(a.machine, &pushgatewaySender{alerter: a, url: url})
		}
	}

	return a
}

// handleAlerts notifies every configured channel.
func (a *alerter) handleAlerts(res *testResults) {
	// There is no text channel to notify, so the message that would have
	// gone to one is printed instead --- which is also where a run on this
	// machine is usually read from.
	a.log.Debug("no chat notification receivers configured, no notifications will be sent")

	if len(a.machine) == 0 {
		a.log.Debug("no machine interface payload receivers configured, no notifications will be sent")

		return
	}

	results := &Results{Map: res.results}

	for _, sender := range a.machine {
		if err := sender.send(results, res.failedCount); err != nil {
			a.log.Errorf("failed to send notification using %s: %v", sender.name(), err)
		}
	}
}

// quiet reports whether there is nothing to say: no check failed, and this
// kind of channel was not asked to hear about successes.
func (a *alerter) quiet(failedCount int, channel string) bool {
	if failedCount > 0 || a.cfg.MachineNotifySuccess {
		return false
	}

	a.log.Debugf("no sockets failed, nothing will be sent to %s", channel)

	return true
}

// submit sends one request and checks that the response says it was accepted.
func (a *alerter) submit(method, url string, body []byte, contentType string, header map[string]string) error {
	res, err := a.stack.Do(r2net.Request{
		Method:      method,
		URL:         url,
		Body:        body,
		ContentType: contentType,
		Header:      header,
		UserAgent:   "dish/" + agentVersion,
	}, time.Duration(a.cfg.TimeoutSeconds)*time.Second)
	if err != nil {
		return err
	}

	if res.Status < 200 || res.Status >= 300 {
		if len(res.Body) > 0 {
			a.log.Warnf("response from %s: %s", url, string(res.Body))
		}

		return fmt.Errorf("unexpected response code received (expected: 200-299, got: %d)", res.Status)
	}

	return nil
}

//
//  Remote API
//

type apiSender struct {
	*alerter

	url         string
	headerName  string
	headerValue string
}

func (s *apiSender) name() string { return "remote API" }

func (s *apiSender) send(results *Results, failedCount int) error {
	if s.quiet(failedCount, "remote API") {
		return nil
	}

	payload, err := json.Marshal(results)
	if err != nil {
		return fmt.Errorf("failed to marshal JSON: %w", err)
	}

	s.log.Debugf("prepared remote API data: %s", payload)

	header := map[string]string{}
	if s.headerName != "" && s.headerValue != "" {
		header[s.headerName] = s.headerValue
	}

	if err := s.submit("POST", s.url, payload, "application/json", header); err != nil {
		return fmt.Errorf("error pushing results to remote API: %w", err)
	}

	s.log.Info("results pushed to remote API")

	return nil
}

//
//  Webhook
//

type webhookSender struct {
	*alerter

	url string
}

func (s *webhookSender) name() string { return "webhook" }

func (s *webhookSender) send(results *Results, failedCount int) error {
	if s.quiet(failedCount, "webhook") {
		return nil
	}

	payload, err := json.Marshal(results)
	if err != nil {
		return err
	}

	s.log.Debugf("prepared webhook data: %s", payload)

	if err := s.submit("POST", s.url, payload, "application/json", nil); err != nil {
		return fmt.Errorf("error pushing results to webhook: %w", err)
	}

	s.log.Info("results pushed to webhook")

	return nil
}

//
//  Pushgateway
//

type pushgatewaySender struct {
	*alerter

	url string
}

func (s *pushgatewaySender) name() string { return "Pushgateway" }

// message is the exposition-format body Pushgateway expects.
//
// Upstream renders this from a text/template.  A template engine on r2 means
// dragging in reflection and a parser to produce four lines of text that never
// vary, so the lines are written out here instead.  The trailing newline is
// not decoration: Pushgateway rejects a body whose last metric is not
// terminated.
func (s *pushgatewaySender) message(failedCount int) string {
	return "\n#HELP failed sockets registered by dish\n" +
		"#TYPE dish_failed_count counter\n" +
		"dish_failed_count " + strconv.Itoa(failedCount) + "\n\n"
}

func (s *pushgatewaySender) send(_ *Results, failedCount int) error {
	if s.quiet(failedCount, "Pushgateway") {
		return nil
	}

	url := s.url + "/metrics/job/" + jobName + "/instance/" + s.cfg.InstanceName

	if err := s.submit("PUT", url, []byte(s.message(failedCount)), "application/byte", nil); err != nil {
		return fmt.Errorf("error pushing results to Pushgateway: %w", err)
	}

	s.log.Info("results pushed to Pushgateway")

	return nil
}

//
//  Formatting
//

// formatMessengerText renders one result as a line of the report.
//
// Upstream marks the two outcomes with emoji.  The kernel's console writes
// bytes into a VGA text buffer in code page 437, so a multi-byte character
// arrives as two or three pieces of line-drawing rubbish; these are the same
// two states said in ASCII.
func formatMessengerText(result Result) string {
	text := "* " + result.Socket.Host + ":" + strconv.Itoa(result.Socket.Port)

	if result.Socket.PathHTTP != "" {
		text += result.Socket.PathHTTP
	}

	if result.Passed {
		return text + " -- success [ OK ]\n"
	}

	text += " -- failed [FAIL]"

	if result.Error != nil {
		text += " -- " + result.Error.Error()
	}

	return text + "\n"
}

// plainHTTPURL validates a channel URL and explains the two ways it can be
// wrong, since one of them --- https --- is a property of this machine rather
// than of the URL.
func plainHTTPURL(raw string) (string, error) {
	u, err := r2net.ParseURL(raw)
	if err != nil {
		return "", fmt.Errorf("error parsing URL %q: %w", raw, err)
	}

	if u.Scheme != "http" {
		return "", fmt.Errorf("unsupported protocol in URL %q: r2 has no TLS, so only http:// endpoints can be reached", raw)
	}

	return raw, nil
}
