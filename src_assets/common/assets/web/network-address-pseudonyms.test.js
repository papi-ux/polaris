import { describe, expect, it } from 'vitest'
import { NetworkAddressBook, pseudonymizeNetworkAddresses } from './network-address-pseudonyms.js'

describe('network address pseudonyms', () => {
  it('replaces a Tailscale client address in a real session line', () => {
    // The line that carried the diagnosis in a real support bundle: the kind of
    // address is what mattered, not the address itself.
    const line = '[2026-09-17 22:02:52.694]: Info: Session started for [Steamdeck] from 100.109.196.18 [active sessions: 1]'

    expect(pseudonymizeNetworkAddresses(line)).toBe(
      '[2026-09-17 22:02:52.694]: Info: Session started for [Steamdeck] from [cgnat-1] [active sessions: 1]'
    )
  })

  it('keeps each address on one label and tells different ones apart', () => {
    expect(pseudonymizeNetworkAddresses('from 192.168.1.192, then 192.168.1.135, then 192.168.1.192 again'))
      .toBe('from [lan-1], then [lan-2], then [lan-1] again')
  })

  it('labels each kind of address by what it is', () => {
    const text = [
      '10.0.0.4', '172.20.1.9', '192.168.0.2', '100.64.0.1', '169.254.3.4', '8.8.8.8',
    ].join(' ')

    expect(pseudonymizeNetworkAddresses(text))
      .toBe('[lan-1] [lan-2] [lan-3] [cgnat-1] [link-local-1] [public-1]')
  })

  it('leaves addresses that identify nothing readable', () => {
    const text = 'loopback 127.0.0.1 any 0.0.0.0 mdns 224.0.0.251 broadcast 255.255.255.255 example 192.0.2.10'

    expect(pseudonymizeNetworkAddresses(text)).toBe(text)
  })

  it('does not mistake versions, timestamps, scopes or MAC addresses for addresses', () => {
    const text = [
      'Polaris version: 1.4.9.8044d371',
      'dotted 1.2.3.4.5',
      '[2026-09-17 21:25:01.845]: Info: started',
      'platf::capture_e::reinit and wl::extcopy_t',
      'mac aa:bb:cc:dd:ee:ff',
    ].join('\n')

    expect(pseudonymizeNetworkAddresses(text)).toBe(text)
  })

  it('keeps a port and a sentence period that follow an address', () => {
    expect(pseudonymizeNetworkAddresses('listening on 192.168.1.5:47989. Done at 8.8.4.4.'))
      .toBe('listening on [lan-1]:47989. Done at [public-1].')
  })

  it('handles IPv6, including a bracketed address with a zone and a port', () => {
    const text = 'v6 [fe80::1%eth0]:47989 tailnet fd7a:115c:a1e0::5 public 2606:4700::1111 self ::1'

    expect(pseudonymizeNetworkAddresses(text))
      .toBe('v6 [link-local-1]:47989 tailnet [tailscale-1] public [public-1] self ::1')
  })

  it('gives an IPv4-mapped address the same label as its dotted form', () => {
    expect(pseudonymizeNetworkAddresses('mapped ::ffff:192.168.1.192 and plain 192.168.1.192'))
      .toBe('mapped [lan-1] and plain [lan-1]')
  })

  it('is idempotent', () => {
    const once = pseudonymizeNetworkAddresses('from 192.168.1.192 and 100.109.196.18')

    expect(pseudonymizeNetworkAddresses(once)).toBe(once)
  })

  it('numbers past labels a previous pass already wrote', () => {
    // Nova redacts before posting and the host redacts again. Reusing [lan-1]
    // for a new address would make a reader take two devices for one.
    expect(pseudonymizeNetworkAddresses('already [lan-1] here, new 10.9.9.9'))
      .toBe('already [lan-1] here, new [lan-2]')
  })

  it('shares labels across calls that use one book', () => {
    const book = new NetworkAddressBook()

    expect(pseudonymizeNetworkAddresses('first 10.0.0.7', book)).toBe('first [lan-1]')
    expect(pseudonymizeNetworkAddresses('second 10.0.0.8 then 10.0.0.7', book)).toBe('second [lan-2] then [lan-1]')
  })

  it('returns empty and missing text unchanged', () => {
    expect(pseudonymizeNetworkAddresses('')).toBe('')
    expect(pseudonymizeNetworkAddresses(null)).toBe('')
    expect(pseudonymizeNetworkAddresses(undefined)).toBe('')
  })
})
