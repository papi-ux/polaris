import { existsSync, readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const currentNotes = () => read('docs/release-notes/v1.4.14.md')

const section = (notes, heading, next) => {
  const start = notes.indexOf(heading)
  const end = notes.indexOf(next, start)
  expect(start).toBeGreaterThanOrEqual(0)
  expect(end).toBeGreaterThan(start)
  return notes.slice(start, end)
}

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

const debugAssets = [
  'Polaris-debug-arch-x86_64.pkg.tar.zst',
  'Polaris-debug-steamos3.8-x86_64.pkg.tar.zst',
]

describe('v1.4.14 release contract', () => {
  // These four are what the source says it builds, not what the last release was. Two of them are
  // only read during a package build, so a mismatch surfaces as a failed SteamOS job rather than as
  // a failed test, which is why they are pinned here where a local run sees them. They moved here
  // from the v1.4.13 contract when 1.4.14 opened.
  it('pins the version every packaging surface agrees on', () => {
    expect(read('CMakeLists.txt')).toContain('project(Polaris VERSION 1.4.14')
    expect(read('docs/benchmark-control-openapi.json')).toContain('"collector_version": "1.4.14"')
    expect(read('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')).toContain(
      'usr/bin/polaris-1.4.14',
    )
    expect(read('scripts/ci/build-steamos-package.sh')).toContain('EXPECTED_PKGVER="1.4.14${POLARIS_PRERELEASE_LABEL}-1"')
  })

  // The release workflow refuses a tag whose notes file is missing or empty, and a beta reads the
  // notes of the release it precedes, so this file has to exist from the moment the version opens.
  it('has curated notes a beta can publish', () => {
    const notes = currentNotes()
    expect(notes.split('\n')[0]).toBe('# Polaris v1.4.14')
    expect(notes.trim().length).toBeGreaterThan(0)
  })

  it('ships exactly the four supported packages and installs them from this tag', () => {
    const notes = currentNotes()
    for (const asset of expectedAssets) {
      expect(notes).toContain(asset)
      expect(notes).toContain(
        `https://github.com/papi-ux/polaris/releases/download/v1.4.14/${asset}`,
      )
    }
    const assetsLine = notes.split('\n').find((line) => line.startsWith('**Assets:**'))
    expect(assetsLine).toBeDefined()
    for (const asset of expectedAssets) {
      expect(assetsLine).toContain(asset)
    }
  })

  it('offers the capture helper for every distro, and the debug packages Arch and SteamOS ship', () => {
    // A release asset is the only way to install the helper on SteamOS and Ubuntu, which have no
    // package repository. The debug packages have shipped since 1.4.13, whose notes never said so.
    const notes = currentNotes()
    for (const asset of expectedAssets) {
      expect(notes).toContain(asset.replace('Polaris-', 'Polaris-kms-'))
    }
    const debugLine = notes.split('\n').find((line) => line.startsWith('**Debug symbols'))
    expect(debugLine).toBeDefined()
    for (const asset of debugAssets) {
      expect(debugLine).toContain(asset)
    }
  })

  // A beta publishes these notes, and the stable tag publishes them again as they stand then.
  // scripts/check-stable-release-notes.py refuses a stable tag that still holds the beta line, but
  // it reads wording, not meaning, so the line has to be the template's own words: a rewording it
  // does not know would reach the stable page. When the beta line comes out for the stable tag,
  // this test goes with it.
  it("tells a beta tester which packages to use in the template's exact words", () => {
    const template = read('docs/release-notes/TEMPLATE.md')
    const lines = template.split('\n').filter((line) => line.includes('X.Y.Z is in beta'))
    expect(lines).toHaveLength(1)
    const betaLine = lines[0].replace('X.Y.Z', '1.4.14').replace(/ \(Beta notes only[^)]*\)$/, '')
    expect(betaLine).toBe(
      '- While 1.4.14 is in beta, use the packages attached to the prerelease you are reading. ' +
        'The install commands below name the final release, which does not exist yet.',
    )
    expect(currentNotes().split('\n')).toContain(betaLine)
  })

  // Nova ships on its own schedule, and nothing at tag time can show that a Nova release named
  // here is out, which is how the 1.4.13 notes came to claim a pair that was not.
  it('never calls the release matched with a Nova release', () => {
    const notes = currentNotes()
    expect(notes).not.toMatch(/matched\s+(?:(?:with|to)\s+)?nova/i)
    expect(notes).not.toMatch(/matching\s+nova\s+(?:release|version|v?\d)/i)
    expect(notes).not.toMatch(/matched\s+pair/i)
  })

  // Moonlight users read the release page too, and Play Setup is a Nova screen they do not have.
  it('sends no reader to a screen only Nova has', () => {
    expect(currentNotes()).not.toContain('Play Setup')
  })

  // Each of these changes what a host does for someone who changed nothing, so each has to be in
  // the heads up, with the setting that brings the old behaviour back where there is one.
  it('names in the heads up what changes under someone who updates', () => {
    const headsUp = currentNotes().slice(currentNotes().indexOf('**Heads up'))
    for (const fact of [
      'On AMD, Auto now tries Vulkan Video first on Gamescope Stream through the portal',
      'It costs AV1 and HDR there, and a client that picked AV1 fails as it starts when its ' +
        'launch switches into Gamescope Stream from another mode.',
      '`encoder = vaapi` or `av1_mode = 2` keeps VA-API.',
      '`capture = kwin` now captures through the portal, and `capture = drm` through KMS.',
      '`adaptive_bitrate_max` limits nothing now.',
      'move a cap you relied on to `max_bitrate`, which now caps live changes too.',
      'A live bitrate set by hand turns Live Tuning off for that stream only',
      '`polaris-kms` needs its exact Polaris: install both files in one command.',
      // 1.4.13 built the Polaris binary without LTO in every package, not only on Arch and
      // SteamOS: the IPO switch meant for the vendored PyroWave trees was set in the scope
      // that creates the polaris target.
      'Every package builds Polaris with link time optimisation again, as before 1.4.13.',
    ]) {
      expect(headsUp).toContain(fact)
    }
  })

  // 1.4.13 shipped six changes its notes never mentioned. This release is where a reader finds out.
  it('announces the six changes 1.4.13 shipped without a word', () => {
    const carried = section(currentNotes(), '**Shipped in 1.4.13, announced now**', '**Heads up')
    for (const fact of [
      '`100.64.0.0/10`',
      'streaming uses `wan_encryption_mode`',
      'Heroic finds installed Amazon and sideloaded games',
      'report their platform and runtime without a re-import',
      'a Spaces checks section',
      'Mission Control shows running Space sessions',
      'Arch and SteamOS releases carry matching debug packages',
    ]) {
      expect(carried).toContain(fact)
    }
  })

  // #189, the private app teardown, was not on staging when these notes were first written, and
  // its line waited in an HTML comment, which a release page does not show, so a tag cut without
  // #189 could publish nothing unfinished. The line is visible exactly when #189's code is in the
  // tree, and no placeholder is left either way.
  it('announces the private app teardown exactly when its code ships', () => {
    const notes = currentNotes()
    const visible = notes.replace(/<!--[\s\S]*?-->/g, '')
    expect(visible).not.toContain('PLACEHOLDER')
    const teardown = visible
      .split('\n')
      .filter((line) => line.includes('Ending a Private Stream quits the app the way a player would'))
    if (existsSync(join(process.cwd(), 'src/platform/linux/private_app_stop.h'))) {
      expect(teardown).toHaveLength(1)
      expect(teardown[0].startsWith('- ')).toBe(true)
      expect(notes).not.toContain('PLACEHOLDER')
    } else {
      expect(teardown).toHaveLength(0)
    }
  })

  // Every figure is quoted with what it assumes. The PyroWave advice figures are the rows the
  // reference page computes from the model, so the two cannot drift apart unseen.
  it('quotes each number with the conditions it holds under', () => {
    const notes = currentNotes()
    const reference = read('docs/pyrowave-reference.md')
    expect(reference).toContain('| 1920x1080 at 60 fps, 4:2:0 | 101 Mbps | 246 Mbps |')
    expect(reference).toContain('| 1920x1080 at 60 fps, 4:4:4 | 109 Mbps | 298 Mbps |')
    expect(notes).toContain(
      "At 1920x1080 and 60 fps in 4:2:0 that's about 101 Mbps on the device's own screen and 246 " +
        'on a TV or monitor (109 and 298 in 4:4:4), at the default 10% FEC with stereo audio.',
    )
    // The own screen target is calibrated to the Retroid Pocket 6, and the notes quote its row.
    expect(reference).toContain('| 1920x1080 at 120 fps, 4:4:4 | 215 Mbps | 594 Mbps |')
    expect(notes).toContain('looked right at 200 Mbps. The figure there is 215, so 200 reads healthy.')
    // A stream is starved more than a tenth below the figure; the byte budget share no longer makes it so.
    expect(notes).toContain('runs more than a tenth below the lower of those figures')
    expect(notes).not.toContain('80% of its recent frames')
    // Held below the model by the cap or max_bitrate with the budget full, Doctor suggests a smaller
    // picture or HEVC, and where only its 300 holds the stream, a bitrate set by hand. The notes quote
    // the 2160p120 row the reference computes.
    expect(reference).toContain('| 3840x2160 at 120 fps, 4:4:4 | 328 Mbps | 699 Mbps |')
    expect(notes).toContain("as at 3840x2160 and 120 fps in 4:4:4 on the device's own screen")
    expect(notes).toContain("Doctor says so and suggests a lower resolution or frame rate, or HEVC. If only Doctor's 300 stands in the way, it adds that you can set more yourself.")
    expect(notes).not.toContain("It doesn't offer to change the bitrate there.")
    // A live bitrate applies at the encoder, so Doctor names a lower live figure while Live Tuning is on.
    expect(notes).toContain('With Live Tuning on it names the live bitrate instead, a little lower, because a live change skips the FEC and audio.')
    // The host takes up to 500 Mbps by hand and announces it; nothing it recommends goes past 300.
    expect(notes).toContain("Polaris takes a bitrate you set yourself up to 500 Mbps now, up from 300, and tells your client it does. Doctor's raise, and anything else Polaris recommends on its own, still stops at 300.")
    expect(notes).not.toContain('wherever your client sends its own bitrate')
    expect(notes).toContain('NVENC with `nvenc_vbv_increase` at 258% or more no longer overflows its buffer at 500 Mbps.')
    expect(notes).toContain('never past 300 Mbps or your `max_bitrate`')
    // Live Tuning lifts a request below adaptive_bitrate_min to that floor, so Doctor is not the only lift.
    expect(notes).toContain('Live Tuning lifting a request below `adaptive_bitrate_min` (2 Mbps by default) to that floor')
    // Live Tuning is on unless a host turned it off, and while it is on Doctor's PyroWave
    // finding is advice to set a bitrate, not the one tap raise.
    expect(notes).toContain('With Live Tuning on, which is the default, Doctor says what to set instead.')
    // A tunnel that carries Ethernet, ZeroTier or an OpenVPN tap, still reports its own MAC.
    expect(notes).toContain('another tunnel with no MAC of its own')
    expect(notes).toContain("the TV figure hasn't been checked on a big screen yet, and Doctor's 300 Mbps ceiling may move")
    expect(notes).toContain("9 ms a frame against VA-API's 16 on one tester's RX 9070 XT at 4K60")
    // Doctor's verdict holds over the network judge's window, and its band, which Live Tuning shares.
    expect(notes).toContain(
      'It now judges video frame loss and round trip time over the last 20 seconds, calls loss network ' +
        'pressure at 2% and clears it below 1%, and Live Tuning cuts for loss from that same point.',
    )
    expect(notes).toContain('the share of video frames that never arrived whole after FEC.')
    // Live Tuning's hold for moderate loss, adaptive_bitrate's MODERATE_LOSS_FLOOR_SHARE and HEAVY_LOSS_PCT,
    // and the rate tests that let it go lower or climb back.
    expect(notes).toContain('For loss of 5% or less it stops at half your bitrate and tests that rate')
    // Media reports that stop no longer leave Live Tuning cutting on the last loss they brought.
    expect(notes).toContain("or of your client's reports stopping")
  })
})
