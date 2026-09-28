import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const read = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const historicalNotes = () => read('docs/release-notes/v1.4.13.md')

const expectedAssets = [
  'Polaris-arch-x86_64.pkg.tar.zst',
  'Polaris-fedora44-x86_64.rpm',
  'Polaris-steamos3.8-x86_64.pkg.tar.zst',
  'Polaris-ubuntu24.04-x86_64.deb',
].sort()

describe('historical v1.4.13 release contract', () => {
  it('ships exactly the four supported packages and installs them from this tag', () => {
    const notes = historicalNotes()
    for (const asset of expectedAssets) {
      expect(notes).toContain(asset)
      expect(notes).toContain(
        `https://github.com/papi-ux/polaris/releases/download/v1.4.13/${asset}`,
      )
    }
    const assetsLine = notes.split('\n').find((line) => line.startsWith('**Assets:**'))
    expect(assetsLine).toBeDefined()
    for (const asset of expectedAssets) {
      expect(assetsLine).toContain(asset)
    }
  })

  // The v1.4.13 page is not published from this copy: the release job reads the tag's own copy,
  // which kept what the betas told their testers, so the page is corrected by posting its body by
  // hand. This copy has to say what shipped because the next release's notes and contract test start
  // from it. The release shares its version with the betas, so a host that ran one can find its
  // package manager treating the release as installed: the notes say how to put it over the beta.
  it('speaks to someone installing the release, not to a beta tester', () => {
    const notes = historicalNotes()
    expect(notes).not.toContain('While 1.4.13 is in beta')
    expect(notes).not.toContain('the prerelease you are reading')
    expect(notes).not.toContain('which does not exist yet')
    expect(notes).toContain(
      '**If this host ran 1.4.13-beta.2 or beta.3, reinstall this release over it.**',
    )
    expect(notes).toContain('sudo dnf reinstall ./Polaris-fedora44-x86_64.rpm')
    expect(notes).toContain('sudo apt install --reinstall ./Polaris-ubuntu24.04-x86_64.deb')
    expect(notes).toContain('On SteamOS, run the install block below again.')
    expect(notes).toContain('on the second and third betas a stream could take the host down')
  })

  // Nova 1.4.13 went stable on 2026-09-27, the day after Polaris 1.4.13, and the stable notes gate
  // still refuses "matched" wording. PyroWave sits in a different place in each Nova build: the
  // Android beta (Nova Pre, beta.3) has it only in Settings, because its Play Setup had no codec row
  // yet; stable Android compiles it out; Linux needs the separate bundle. A reader who looked in
  // Play Setup found nothing, so the notes name each path.
  it('says where PyroWave is in each Nova build without calling the two a matched release', () => {
    const notes = historicalNotes()
    expect(notes).not.toMatch(/matched with Nova/i)
    expect(notes).not.toContain('Nova 1.4.13 is still in beta')
    expect(notes).toContain('Nova 1.4.13 is out too!')
    expect(notes).toContain(
      'Settings → Client Stream Defaults → Change codec settings → PyroWave (experimental)',
    )
    expect(notes).toContain("Nova's stable 1.4.13 APKs don't turn it on")
    expect(notes).toContain('`Nova-Linux-PyroWave-x86_64-alpha.flatpak`')
    expect(notes).toContain('Play Setup → Video Codec → PyroWave · Experimental')
  })

  // 0.49 ms was one host encoding a frame that never left the GPU. Private Stream, wlroots and X11
  // capture hand PyroWave frames in host memory, which pay an upload first, and HDR on this codec
  // had not been shown end to end, so the page quotes what a recorded session paid instead.
  it('quotes the PyroWave cost that host memory capture pays', () => {
    const notes = historicalNotes()
    expect(notes).not.toContain('0.49 ms')
    expect(notes).not.toContain('without the frame ever leaving the GPU')
    expect(notes).toContain('that averaged 0.35 ms for the copy plus 0.95 ms to encode')
    expect(notes).not.toContain('HDR works when capture goes through the desktop portal')
    expect(notes).toContain('HDR on this codec has not been shown end to end yet.')
  })

  it('stops warning that an update takes the KMS capture permission away', () => {
    // Every release from 1.4.9 to 1.4.12 told people that installing or updating removes the KMS
    // capture permission, because it did. A package owns it now, so repeating that warning would
    // send someone to re-run a command they no longer need, and would hide the one thing this
    // release actually asks of them: install polaris-kms, and log out once.
    const notes = historicalNotes()
    expect(notes).not.toContain('removes the KMS capture permission')
    expect(notes).toContain('polaris-kms')
    expect(notes).toContain('sudo -H polaris --setup-host --enable-kms')
    expect(notes).toContain('log out')
  })

  it('offers the capture helper for every distro it ships Polaris for', () => {
    // A release asset is the only way to install the helper on SteamOS and Ubuntu, which have no
    // package repository, so leaving one out silently removes DRM/KMS capture from those hosts.
    const notes = historicalNotes()
    for (const asset of expectedAssets) {
      expect(notes).toContain(asset.replace('Polaris-', 'Polaris-kms-'))
    }
  })
})
