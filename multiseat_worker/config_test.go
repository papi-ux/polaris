package main

import "testing"

func validEnvironment() map[string]string {
	return map[string]string{
		"POLARIS_CONTROLLER_EPOCH":        "controller-a1b2",
		"POLARIS_LOGICAL_GPU_ID":          "gpu-primary",
		"POLARIS_WORKER_NAME":             "polaris-worker-controller-a1b2-42",
		"POLARIS_SEAT_SLOT":               "7",
		"POLARIS_SEAT_GENERATION":         "42",
		"POLARIS_RUNTIME_NAMESPACE":       "polaris-runtime-controller-a1b2-42",
		"POLARIS_CAPTURE_WAYLAND_DISPLAY": "polaris-capture-controller-a1b2-42",
		"WAYLAND_DISPLAY":                 "polaris-wayland-controller-a1b2-42",
		"PULSE_SINK":                      "polaris-audio-controller-a1b2-42",
		"POLARIS_INPUT_SEAT":              "polaris-input-controller-a1b2-42",
		"POLARIS_RENDER_NODE":             "/dev/dri/renderD128",
		"POLARIS_COMPOSITOR":              "gamescope",
		"POLARIS_RUNTIME_PROFILE":         "steam",
		"POLARIS_DISPLAY_TOPOLOGY":        "capture-host-with-nested-compositor",
		"POLARIS_MEDIA_PIPELINE":          "worker-local-capture-encode",
		"POLARIS_DISPLAY_WIDTH":           "3840",
		"POLARIS_DISPLAY_HEIGHT":          "2160",
		"POLARIS_DISPLAY_REFRESH_MILLIHZ": "97000",
		"POLARIS_DISPLAY_HDR":             "1",
		"POLARIS_ENCODER_SESSIONS":        "1",
	}
}

func mapLookup(values map[string]string) func(string) (string, bool) {
	return func(key string) (string, bool) {
		value, present := values[key]
		return value, present
	}
}

func TestWorkerConfigRequiresCanonicalImmutableAllocation(t *testing.T) {
	plan := workloadPlan{Kind: workloadKindSteam, TargetID: "steam-game"}
	config, err := loadWorkerConfig(mapLookup(validEnvironment()), plan)
	if err != nil {
		t.Fatal(err)
	}
	if config.Identity != goldenIdentity() || config.Compositor != "gamescope" ||
		config.RuntimeProfile != "steam" || config.DisplayWidth != 3840 ||
		config.DisplayHeight != 2160 || config.RefreshMillihz != 97000 ||
		!config.DisplayHDR || config.EncoderSessions != 1 || config.Workload != plan {
		t.Fatalf("unexpected worker config: %+v", config)
	}
	if _, err := loadWorkerConfig(
		mapLookup(validEnvironment()),
		workloadPlan{Kind: workloadKindHeroic, TargetID: "heroic-game"},
	); err == nil {
		t.Fatal("runtime-mismatched workload plan was accepted")
	}

	tests := map[string]func(map[string]string){
		"missing identity": func(values map[string]string) { delete(values, "POLARIS_WORKER_NAME") },
		"zero generation":  func(values map[string]string) { values["POLARIS_SEAT_GENERATION"] = "0" },
		"leading zero":     func(values map[string]string) { values["POLARIS_SEAT_SLOT"] = "07" },
		"automatic mode":   func(values map[string]string) { values["POLARIS_COMPOSITOR"] = "automatic" },
		"unknown profile":  func(values map[string]string) { values["POLARIS_RUNTIME_PROFILE"] = "automatic" },
		"unknown topology": func(values map[string]string) { values["POLARIS_DISPLAY_TOPOLOGY"] = "nested" },
		"unknown pipeline": func(values map[string]string) { values["POLARIS_MEDIA_PIPELINE"] = "gstreamer" },
		"zero width":       func(values map[string]string) { values["POLARIS_DISPLAY_WIDTH"] = "0" },
		"huge height":      func(values map[string]string) { values["POLARIS_DISPLAY_HEIGHT"] = "16385" },
		"low refresh":      func(values map[string]string) { values["POLARIS_DISPLAY_REFRESH_MILLIHZ"] = "999" },
		"localized refresh": func(values map[string]string) {
			values["POLARIS_DISPLAY_REFRESH_MILLIHZ"] = "97,000"
		},
		"invalid HDR":      func(values map[string]string) { values["POLARIS_DISPLAY_HDR"] = "true" },
		"relative device":  func(values map[string]string) { values["POLARIS_RENDER_NODE"] = "dev/dri/renderD128" },
		"resource escape":  func(values map[string]string) { values["POLARIS_INPUT_SEAT"] = "../seat" },
		"capture escape":   func(values map[string]string) { values["POLARIS_CAPTURE_WAYLAND_DISPLAY"] = "../capture" },
		"encoder overflow": func(values map[string]string) { values["POLARIS_ENCODER_SESSIONS"] = "65" },
	}
	for name, mutate := range tests {
		t.Run(name, func(t *testing.T) {
			values := validEnvironment()
			mutate(values)
			if _, err := loadWorkerConfig(mapLookup(values), plan); err == nil {
				t.Fatal("invalid worker allocation was accepted")
			}
		})
	}
}

func TestRunArgumentsAreLiteralAndBounded(t *testing.T) {
	want := workloadPlan{Kind: workloadKindSteam, TargetID: "steam-game"}
	if got, err := parseRunArguments([]string{"--workload-kind=steam", "--workload-id=steam-game"}); err != nil || got != want {
		t.Fatalf("literal workload rejected: %q, %v", got, err)
	}
	for _, arguments := range [][]string{
		nil,
		{"--workload-kind=steam"},
		{"--workload-kind=steam", "--workload-id="},
		{"--workload-kind=unknown", "--workload-id=game"},
		{"--workload-id=game", "--workload-kind=steam"},
		{"--workload-kind=steam", "--workload-id=line\nbreak"},
		{"--workload-kind=steam", "--workload-id=game;command"},
	} {
		if _, err := parseRunArguments(arguments); err == nil {
			t.Fatalf("invalid arguments were accepted: %q", arguments)
		}
	}
}
