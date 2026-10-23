# Polaris Nix packaging

## Layers

| Layer | Module | Owns |
|-------|--------|------|
| **NixOS** | `nixosModules.polaris` | udev, firewall, groups, packages |
| **hjem** | `hjemModules.polaris` | per-user units + packages |
| **home-manager** | `homeModules.polaris` | same via `systemd.user` |

Importing the NixOS module reserves `services.polaris` for the game-streaming
host and disables nixpkgs' unrelated Polaris music-server module, which uses the
same option name. The two service modules cannot share that namespace.

GPU selection is **only** Polaris web UI `adapter_name` (or leave empty for auto).  
No hybrid “nvidia pin” rewrite of conf.

## Packages

- `polaris-stream` — builds from this repo (standalone CUDA/NVENC default; overlay follows host `cudaSupport`)
- `gamescope-polaris` — Valve gamescope + HDR PW patches (`gamescope-hdr` alias)
- `xdg-desktop-portal-gamescope` — private ScreenCast backend

## GPU build and first-run defaults

The standalone flake keeps its CUDA-enabled package default. A host importing
our overlay now honors its own `nixpkgs.config.cudaSupport`; to explicitly
build without CUDA, use:

```nix
services.polaris.package = pkgs.polaris-stream.override { cudaSupport = false; };
```

Nixpkgs defaults CUDA support off. NVIDIA hosts importing the overlay that
previously relied on its always-enabled CUDA build should set
`nixpkgs.config.cudaSupport = true`, or choose it for this package with
`services.polaris.package = pkgs.polaris-stream.override { cudaSupport = true; };`.

Both variants keep Vulkan headers and the runtime loader. This makes Vulkan
available to the non-CUDA build; it does not install a GPU driver or establish
AMD/Intel streaming support on a particular host.

A new per-user configuration leaves the encoder on automatic selection.
The `streamMode = "labwc"` option seeds Polaris's `headless_stream` mode;
`gamescope_stream` keeps its existing mode. Existing `polaris.conf` files are
left alone, so check an older explicit `encoder = nvenc` setting in the web UI
when moving a configuration to another GPU. The configured Gamescope helper
also takes priority over the package's copy in Home Manager/hjem profiles.

The package version follows Polaris's CMake project version. Git-backed flake
inputs report their source revision; a path-only input reports an unknown
commit rather than a fixed historical commit.

## CI coverage

`.github/workflows/nix.yml`, scoped by cost:

| Runs | Covers |
|------|--------|
| every push (Public Hygiene) | hunk headers and patch declarations, offline |
| changes under `nix/` or `flake.*` | the patch stacks against their pinned sources; `nix flake check`; every package evaluates |
| the same, then | `nix build` of `gamescope-polaris` and the portal — the only place the `+polhdr` install check can run |
| manual and weekly | `nix build` of `polaris-stream`, which is what catches a stale `npmDepsHash` |

The x86_64 flake check also evaluates the NixOS host module with the host enabled
and disabled. It catches collisions with the ordinary nixpkgs module set; it
does not boot NixOS or test streaming.
The GPU-defaults check evaluates both CUDA variants, their Vulkan inputs and
loader wrapper, both generated stream seeds and the configured helper's
profile priority. These assertions do not compile Polaris or run a GPU.
The session-wrapper check builds the configured wrapper with stock Gamescope,
so its real ShellCheck runs without compiling Polaris or the patched compositor.
It never starts the wrapper or a stream.

Not covered: the home-manager module, and evaluation for `aarch64-linux`.

## hjem / home-manager example

Wire the flake overlay for packages, then enable the per-user module. A separate
host module (or your compositor stack) can still own a private gamescope
ScreenCast portal; set `manageCoreUnits = false` on that side if Polaris should
not start portal units itself.

```nix
# inputs
# submodules=1 fetches the third-party sources (incl. PyroWave's Granite/pyrowave);
# the github: shorthand does NOT fetch submodules. Pin a published release with
# ?ref=refs/tags/<release>&submodules=1.
polaris.url = "git+https://github.com/papi-ux/polaris?submodules=1";
polaris.inputs.nixpkgs.follows = "nixpkgs";

# system: overlay + optional nixosModules.polaris (udev / firewall / packages)
nixpkgs.overlays = [ inputs.polaris.overlays.default ];
# NVIDIA/NVENC is opt-in: the overlay follows nixpkgs.config.cudaSupport (default off).
# nixpkgs.config.cudaSupport = true; # needs allowUnfree; omit for a non-CUDA build

# per-user (hjem or home-manager)
imports = [ inputs.polaris.hjemModules.polaris ];
# or: imports = [ inputs.polaris.homeModules.polaris ];
services.polaris = {
  enable = true;
  streamMode = "gamescope_stream"; # default
  # desktopUserTarget = "graphical-session.target"; # default
  # preferVkDevice = "10de:2684"; # optional gamescope --prefer-vk-device
};
```

After rebuild / switch:

```bash
systemctl --user daemon-reload
systemctl --user enable --now polaris-gamescope-idle.service polaris.service
systemctl --user status polaris.service polaris-gamescope-idle.service
```

hjem links units under `~/.config/systemd/user/` **and**
`${desktopUserTarget}.wants/` so the compositor target pulls them in.

### Defaults (one source for hjem + home-manager)

`nix/modules/options.nix` is imported by **NixOS, hjem, and HM** — same options,
same defaults. HM is not CI-tested here; if hjem evaluates, HM should match.

| Option | Default |
|--------|---------|
| `streamMode` | `"gamescope_stream"` |
| `width` / `height` / `refresh` | `3840` / `2160` / `120` |
| `preferVkDevice` | `null` |
| `environment` | `{ }` |
| `desktopUserTarget` | `"graphical-session.target"` |
| packages | flake overlay packages |

Minimal user config is just `services.polaris.enable = true`.

### Environment options vs hjem-rum

**hjem-rum** (`rum.programs.*`) is for user programs (zsh, git, …). Polaris is a
**service**, so env is `services.polaris.*` → unit `Environment=` only (not
`environment.sessionVariables`).

| Option | Effect |
|--------|--------|
| `preferVkDevice` | `POLARIS_GAMESCOPE_PREFER_VK` → gamescope `--prefer-vk-device` |
| `environment` | free-form attrs → extra unit env |
| `width` / `height` / `refresh` | `POLARIS_HDR_*` |
| web UI `adapter_name` | Polaris encode/capture GPU (not unit env) |

## Build

```bash
# third-party/* are git submodules — required for cmake
nix build '.?submodules=1#polaris-stream'
nix build '.?submodules=1#gamescope-polaris'
```

### gamescope patches

See [`nix/patches/gamescope/README.md`](./patches/gamescope/README.md).

| Patch | Upstream |
|-------|----------|
| 01 | [#2270](https://github.com/ValveSoftware/gamescope/pull/2270) xBGR_210LE |
| 06 | [#2217](https://github.com/ValveSoftware/gamescope/pull/2217) discrete GPU |
| 07 | companion to #2270 (`EOTF_PQ` in `paint_pipewire`) |

Delete each when its upstream lands (`rg POLARIS-UPSTREAM-REMOVE nix/patches/gamescope`).
