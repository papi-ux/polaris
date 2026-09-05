package seatruntime

import (
	"encoding/json"
	"reflect"
	"strings"
	"testing"
)

func catalogForTest() Catalog {
	return Catalog{
		Schema: 1,
		Providers: []Provider{
			{Stage: StageSessionBus, Executable: "/usr/libexec/polaris/session-bus"},
			{
				Stage: StageNestedCompositor, Selector: "gamescope",
				Executable: "/usr/libexec/polaris/gamescope-provider",
				Arguments:  []string{"--locked-setting", "value with spaces"},
			},
			{
				Stage: StageLauncher, Selector: "heroic",
				TargetID:   "heroic-catalog-id",
				Executable: "/usr/libexec/polaris/heroic-provider",
				Arguments:  []string{"--catalog=heroic", "$(touch should-remain-literal)"},
			},
		},
	}
}

func TestCatalogDecodeAndResolveUsesExactStageSelector(t *testing.T) {
	encoded, err := json.Marshal(catalogForTest())
	if err != nil {
		t.Fatal(err)
	}
	catalog, err := DecodeCatalog(strings.NewReader(string(encoded)))
	if err != nil {
		t.Fatal(err)
	}
	request := protocolTestRequests()[6]
	plan, err := catalog.Resolve(request)
	if err != nil {
		t.Fatal(err)
	}
	wantArguments := []string{
		"/usr/libexec/polaris/heroic-provider",
		"serve-resource-v1",
		"--stage=launcher-process-tree",
		"--runtime-namespace=seat-7-generation-19",
		"--runtime-profile=heroic",
		"--workload-kind=heroic",
		"--workload-id=heroic-catalog-id",
		"--wayland-socket=polaris-wayland-7",
		"--audio-sink=polaris-seat-7",
		"--input-seat=polaris-input-7",
		"--",
		"--catalog=heroic",
		"$(touch should-remain-literal)",
	}
	if plan.Executable != wantArguments[0] ||
		!reflect.DeepEqual(plan.Arguments, wantArguments) {
		t.Fatalf("resolved provider argv mismatch: %#v", plan)
	}
	expectedEnvironment, err := Environment(request)
	if err != nil {
		t.Fatal(err)
	}
	expectedEnvironment = append(expectedEnvironment, ReadyFDSetting+"=3")
	if !reflect.DeepEqual(plan.Environment, expectedEnvironment) {
		t.Fatalf("resolved provider environment mismatch: %#v", plan.Environment)
	}
	request.RuntimeProfile = "steam"
	request.WorkloadKind = WorkloadSteam
	if _, err := catalog.Resolve(request); err == nil {
		t.Fatal("missing workload provider was accepted")
	}
	request.RuntimeProfile = "heroic"
	request.WorkloadKind = WorkloadHeroic
	request.WorkloadID = "different-catalog-id"
	if _, err := catalog.Resolve(request); err == nil {
		t.Fatal("unlisted workload target was accepted")
	}
}

func TestCatalogResolvesEveryStageThroughOneExactKey(t *testing.T) {
	requests := protocolTestRequests()
	catalog := Catalog{Schema: 1}
	for _, request := range requests {
		provider := Provider{
			Stage:      request.Stage,
			Executable: "/usr/libexec/polaris/provider-" + string(request.Stage),
		}
		switch request.Stage {
		case StageNestedCompositor:
			provider.Selector = request.Compositor
		case StageLauncher:
			provider.Selector = string(request.WorkloadKind)
			provider.TargetID = request.WorkloadID
		}
		catalog.Providers = append(catalog.Providers, provider)
	}
	if err := validateCatalog(catalog); err != nil {
		t.Fatal(err)
	}
	for _, request := range requests {
		plan, err := catalog.Resolve(request)
		if err != nil {
			t.Fatalf("stage %s did not resolve: %v", request.Stage, err)
		}
		if !strings.HasSuffix(plan.Executable, string(request.Stage)) {
			t.Fatalf("stage %s resolved the wrong provider: %q", request.Stage, plan.Executable)
		}
	}
	changed := requests[3]
	changed.Compositor = "labwc"
	if _, err := catalog.Resolve(changed); err == nil {
		t.Fatal("unlisted compositor provider was accepted")
	}
}

func TestCatalogRejectsAmbiguityAndExecutableTemplates(t *testing.T) {
	tests := map[string]func(*Catalog){
		"duplicate selection": func(catalog *Catalog) {
			catalog.Providers = append(catalog.Providers, catalog.Providers[0])
		},
		"relative executable": func(catalog *Catalog) {
			catalog.Providers[0].Executable = "provider"
		},
		"wrong selector": func(catalog *Catalog) {
			catalog.Providers[0].Selector = "gamescope"
		},
		"unknown launcher": func(catalog *Catalog) {
			catalog.Providers[2].Selector = "custom"
		},
		"missing launcher target": func(catalog *Catalog) {
			catalog.Providers[2].TargetID = ""
		},
		"control byte": func(catalog *Catalog) {
			catalog.Providers[0].Arguments = []string{"line\nbreak"}
		},
	}
	for name, mutate := range tests {
		t.Run(name, func(t *testing.T) {
			catalog := catalogForTest()
			mutate(&catalog)
			encoded, err := json.Marshal(catalog)
			if err != nil {
				t.Fatal(err)
			}
			if _, err := DecodeCatalog(strings.NewReader(string(encoded))); err == nil {
				t.Fatal("invalid provider catalog was accepted")
			}
		})
	}
}

func TestCatalogJSONIsStrictAndBounded(t *testing.T) {
	for name, content := range map[string]string{
		"unknown field":   `{"schema":1,"providers":[],"extra":true}`,
		"duplicate field": `{"schema":1,"schema":1,"providers":[]}`,
		"trailing value":  `{"schema":1,"providers":[]} {}`,
		"wrong schema":    `{"schema":2,"providers":[{"stage":"session-bus","executable":"/bin/provider","arguments":[]}]}`,
	} {
		t.Run(name, func(t *testing.T) {
			if _, err := DecodeCatalog(strings.NewReader(content)); err == nil {
				t.Fatal("invalid catalog JSON was accepted")
			}
		})
	}
	tooLarge := strings.Repeat(" ", maximumCatalogBytes+1)
	if _, err := DecodeCatalog(strings.NewReader(tooLarge)); err == nil {
		t.Fatal("oversized provider catalog was accepted")
	}
}
