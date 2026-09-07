package seatruntime

import (
	"bytes"
	"encoding/json"
	"errors"
	"io"
	"path/filepath"
	"strings"
)

const (
	catalogSchemaVersion      = 1
	maximumCatalogBytes       = 1024 * 1024
	maximumCatalogProviders   = 4096
	maximumProviderArguments  = 32
	maximumProviderValueBytes = 4096
)

type Provider struct {
	Stage      Stage    `json:"stage"`
	Selector   string   `json:"selector,omitempty"`
	TargetID   string   `json:"target_id,omitempty"`
	Executable string   `json:"executable"`
	Arguments  []string `json:"arguments"`
}

type Catalog struct {
	Schema    uint32     `json:"schema"`
	Providers []Provider `json:"providers"`
}

type Plan struct {
	Executable  string
	Arguments   []string
	Environment []string
}

func consumeJSONValue(decoder *json.Decoder) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	delimiter, compound := token.(json.Delim)
	if !compound {
		return nil
	}
	switch delimiter {
	case '{':
		seen := make(map[string]struct{})
		for decoder.More() {
			nameToken, err := decoder.Token()
			if err != nil {
				return err
			}
			name, ok := nameToken.(string)
			if !ok {
				return errors.New("runtime provider catalog object is invalid")
			}
			if _, duplicate := seen[name]; duplicate {
				return errors.New("runtime provider catalog contains a duplicate field")
			}
			seen[name] = struct{}{}
			if err := consumeJSONValue(decoder); err != nil {
				return err
			}
		}
		closing, err := decoder.Token()
		if err != nil || closing != json.Delim('}') {
			return errors.New("runtime provider catalog object is invalid")
		}
	case '[':
		for decoder.More() {
			if err := consumeJSONValue(decoder); err != nil {
				return err
			}
		}
		closing, err := decoder.Token()
		if err != nil || closing != json.Delim(']') {
			return errors.New("runtime provider catalog array is invalid")
		}
	default:
		return errors.New("runtime provider catalog JSON is invalid")
	}
	return nil
}

func validateUniqueJSONFields(content []byte) error {
	decoder := json.NewDecoder(bytes.NewReader(content))
	if err := consumeJSONValue(decoder); err != nil {
		return err
	}
	if _, err := decoder.Token(); err != io.EOF {
		return errors.New("runtime provider catalog has trailing content")
	}
	return nil
}

func validProviderValue(value string, allowEmpty bool) bool {
	if (!allowEmpty && value == "") || len(value) > maximumProviderValueBytes {
		return false
	}
	for _, character := range []byte(value) {
		if character < 0x20 || character > 0x7e {
			return false
		}
	}
	return true
}

func validProviderExecutable(path string) bool {
	return validProviderValue(path, false) && filepath.IsAbs(path) &&
		filepath.Clean(path) == path && path != "/" &&
		!strings.ContainsAny(path, "\x00\n\r")
}

func validProviderSelector(provider Provider) bool {
	switch provider.Stage {
	case StageNestedCompositor:
		return validCompositor(provider.Selector) && provider.TargetID == ""
	case StageLauncher:
		return validWorkloadKind(WorkloadKind(provider.Selector)) &&
			validNameToken(provider.TargetID, 128)
	case StageSessionBus, StageAudio, StageDisplayCapture,
		StageVirtualInput, StageEncoder:
		return provider.Selector == "" && provider.TargetID == ""
	default:
		return false
	}
}

func validateCatalog(catalog Catalog) error {
	if catalog.Schema != catalogSchemaVersion || len(catalog.Providers) == 0 ||
		len(catalog.Providers) > maximumCatalogProviders {
		return errors.New("runtime provider catalog header is invalid")
	}
	seen := make(map[string]struct{}, len(catalog.Providers))
	for _, provider := range catalog.Providers {
		if !validProviderSelector(provider) ||
			!validProviderExecutable(provider.Executable) ||
			len(provider.Arguments) > maximumProviderArguments {
			return errors.New("runtime provider catalog entry is invalid")
		}
		for _, argument := range provider.Arguments {
			if !validProviderValue(argument, true) {
				return errors.New("runtime provider catalog argument is invalid")
			}
		}
		key := string(provider.Stage) + "\x00" + provider.Selector + "\x00" + provider.TargetID
		if _, duplicate := seen[key]; duplicate {
			return errors.New("runtime provider catalog selection is ambiguous")
		}
		seen[key] = struct{}{}
	}
	return nil
}

// DecodeCatalog accepts only a single, bounded, strict JSON value. Catalogs
// are data, not shell templates: arguments remain literal argv elements.
func DecodeCatalog(reader io.Reader) (Catalog, error) {
	if reader == nil {
		return Catalog{}, errors.New("runtime provider catalog is missing")
	}
	content, err := io.ReadAll(io.LimitReader(reader, maximumCatalogBytes+1))
	if err != nil || len(content) == 0 || len(content) > maximumCatalogBytes {
		return Catalog{}, errors.New("runtime provider catalog could not be read")
	}
	if err := validateUniqueJSONFields(content); err != nil {
		return Catalog{}, err
	}
	decoder := json.NewDecoder(bytes.NewReader(content))
	decoder.DisallowUnknownFields()
	var catalog Catalog
	if err := decoder.Decode(&catalog); err != nil {
		return Catalog{}, errors.New("runtime provider catalog JSON is invalid")
	}
	if err := validateCatalog(catalog); err != nil {
		return Catalog{}, err
	}
	return catalog, nil
}

func (catalog Catalog) Resolve(request Request) (Plan, error) {
	if err := validateCatalog(catalog); err != nil {
		return Plan{}, err
	}
	if err := validateRequest(request); err != nil {
		return Plan{}, err
	}
	selector := selectorFor(request)
	targetID := ""
	if request.Stage == StageLauncher {
		targetID = request.WorkloadID
	}
	var selected *Provider
	for index := range catalog.Providers {
		provider := &catalog.Providers[index]
		if provider.Stage == request.Stage && provider.Selector == selector &&
			provider.TargetID == targetID {
			selected = provider
			break
		}
	}
	if selected == nil {
		return Plan{}, errors.New("runtime provider is not present in the trusted catalog")
	}
	runtimeArguments, err := Arguments(request)
	if err != nil {
		return Plan{}, err
	}
	runtimeArguments[0] = "serve-resource-v1"
	arguments := make([]string, 0, len(runtimeArguments)+len(selected.Arguments)+2)
	arguments = append(arguments, selected.Executable)
	arguments = append(arguments, runtimeArguments...)
	arguments = append(arguments, "--")
	arguments = append(arguments, selected.Arguments...)
	environment, err := Environment(request)
	if err != nil {
		return Plan{}, err
	}
	environment = append(environment, ReadyFDSetting+"=3")
	return Plan{
		Executable:  selected.Executable,
		Arguments:   arguments,
		Environment: environment,
	}, nil
}
