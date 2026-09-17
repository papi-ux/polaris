import { describe, expect, it } from 'vitest'

import {
  normalizeCidr,
  parseTrustedSubnets,
  serializeTrustedSubnets,
  trustsSubnet,
  withTrustedSubnet,
} from './trusted-network.js'

describe('trusted network helpers', () => {
  it('reads every form a trusted subnet list is saved in', () => {
    expect(parseTrustedSubnets('10.0.0.0/24,192.168.1.0/24')).toEqual(['10.0.0.0/24', '192.168.1.0/24'])
    expect(parseTrustedSubnets('["10.0.0.0/24", "fd00::/64"]')).toEqual(['10.0.0.0/24', 'fd00::/64'])
    expect(parseTrustedSubnets('[10.0.0.0/24, fd00::/64]')).toEqual(['10.0.0.0/24', 'fd00::/64'])
    expect(parseTrustedSubnets("'10.0.0.0/24'\n192.168.1.0/24")).toEqual(['10.0.0.0/24', '192.168.1.0/24'])
    expect(parseTrustedSubnets(['10.0.0.0/24', ' '])).toEqual(['10.0.0.0/24'])
    expect(parseTrustedSubnets('')).toEqual([])
    expect(parseTrustedSubnets(undefined)).toEqual([])
    expect(serializeTrustedSubnets(['10.0.0.0/24', 'fd00::/64'])).toBe('10.0.0.0/24,fd00::/64')
  })

  it('stores a network the pairing check accepts and refuses very broad ones', () => {
    expect(normalizeCidr(' 192.168.1.20/24 ')).toEqual({ cidr: '192.168.1.0/24' })
    expect(normalizeCidr('10.0.0.232/8')).toEqual({ cidr: '10.0.0.0/8' })
    expect(normalizeCidr('172.16.4.20/32')).toEqual({ cidr: '172.16.4.20/32' })
    expect(normalizeCidr('FD00:1234::/64')).toEqual({ cidr: 'fd00:1234::/64' })
    expect(normalizeCidr('0.0.0.0/0')).toEqual({ error: 'too_broad' })
    expect(normalizeCidr('10.0.0.0/7')).toEqual({ error: 'too_broad' })
    expect(normalizeCidr('::/0')).toEqual({ error: 'too_broad' })
    expect(normalizeCidr('192.168.1.300/24')).toEqual({ error: 'invalid' })
    expect(normalizeCidr('192.168.1.0/33')).toEqual({ error: 'invalid' })
    expect(normalizeCidr('192.168.1.0')).toEqual({ error: 'invalid' })
    expect(normalizeCidr('fd00::1::/64')).toEqual({ error: 'invalid' })
    expect(normalizeCidr('home network')).toEqual({ error: 'invalid' })
  })

  it('adds a network once and keeps the entries already trusted as written', () => {
    const saved = ['"192.168.50.0/24"', '10.0.0.5/24']
    expect(trustsSubnet(saved, '192.168.50.0/24')).toBe(true)
    expect(trustsSubnet(saved, '10.0.0.0/24')).toBe(true)
    expect(withTrustedSubnet(saved, '10.0.0.0/24')).toEqual(saved)
    expect(withTrustedSubnet(saved, '172.16.4.0/24')).toEqual([...saved, '172.16.4.0/24'])
  })
})
