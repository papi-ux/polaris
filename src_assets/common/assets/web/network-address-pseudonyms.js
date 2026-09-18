// Network addresses in shared diagnostics.
//
// Text that leaves this machine (the anonymized bundle, a prefilled GitHub issue,
// copied support text, evidence handed to an AI explanation) used to carry every
// address verbatim. Credentials were redacted by name, but an address has no name
// in front of it: `Session started for [Deck] from 100.109.196.18` is the whole
// line. That identifies a home network and, for a tailnet address, a device on it.
//
// Addresses are replaced rather than blanked, because the kind of address is
// often the diagnosis. A client streaming over Tailscale behaves differently from
// one on the same LAN, and a bundle that says only "[redacted]" cannot tell them
// apart. So each address becomes a label that keeps its kind and stays the same
// everywhere in one piece of text, which keeps "the same device reconnected seven
// times" visible without saying which device it was.

// Kinds that carry no identity and read better left alone. Loopback and the
// unspecified address are this machine talking to itself, multicast is a
// protocol group rather than a host, and the documentation ranges only ever
// appear in examples.
const KEPT_KINDS = new Set(['loopback', 'unspecified', 'multicast', 'broadcast', 'documentation'])

// IPv4 in free text. The lookarounds keep it from matching inside something
// longer: a version such as 1.4.9.8044d371 is not an address, and neither is the
// middle of 1.2.3.4.5. A trailing sentence period is allowed; a trailing digit is not.
const IPV4_OCTET = String.raw`(?:25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)`
const IPV4_IN_TEXT_SOURCE = String.raw`(?<![\d.])${IPV4_OCTET}(?:\.${IPV4_OCTET}){3}(?!\.?\d)`

// IPv6 candidates are found loosely and then validated strictly. Log lines are
// full of colon-separated numbers that are not addresses (`21:25:01.845`,
// C++ scopes like `platf::capture_e`), so the pattern only nominates runs of hex,
// colons and dots with at least two colons, and the URL parser decides. A
// bracketed form such as [fe80::1]:47989 is taken whole so the replacement does
// not end up double-bracketed, and a zone such as %eth0 is dropped with it since
// it names an interface on this host.
// The trailing guard applies only when there is no closing bracket, because a
// bracketed address is normally followed by `:port`.
const IPV6_CANDIDATE_SOURCE = String.raw`(?<![\w:.\[])(\[?)((?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f.]*)(%[\w.-]+)?(?:(\])|(?![\w:]))`

// A label this module already wrote, such as [lan-2]. Found so a second pass can
// number past it instead of reusing the same label for a different address.
const LABEL_SOURCE = String.raw`\[(lan|cgnat|tailscale|link-local|public)-(\d+)\]`

function ipv4Octets(address) {
  return address.split('.').map(Number)
}

/**
 * The kind of an IPv4 address, from its first octets.
 *
 * 100.64.0.0/10 is reported as `cgnat` rather than as Tailscale. Tailscale does
 * hand out addresses from it, and on a streaming client that is almost always
 * what it is, but ISPs use the same range for carrier-grade NAT and a label
 * should not claim more than the address says.
 */
function classifyIpv4(address) {
  const [a, b, c] = ipv4Octets(address)
  if (a === 0) return 'unspecified'
  if (a === 127) return 'loopback'
  if (a === 10 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168)) return 'lan'
  if (a === 100 && b >= 64 && b <= 127) return 'cgnat'
  if (a === 169 && b === 254) return 'link-local'
  if (a >= 224 && a <= 239) return 'multicast'
  if (address === '255.255.255.255') return 'broadcast'
  if ((a === 192 && b === 0 && c === 2) || (a === 198 && b === 51 && c === 100) || (a === 203 && b === 0 && c === 113)) {
    return 'documentation'
  }
  return 'public'
}

/**
 * Parse a candidate IPv6 address, returning its canonical form or null.
 *
 * The URL parser is the validator because it is exact and already present in
 * both the browser and the test runtime, where a hand-written IPv6 grammar is
 * the classic source of both false positives and missed forms.
 */
function canonicalIpv6(candidate) {
  if (!candidate.includes(':')) return null
  try {
    const hostname = new URL(`http://[${candidate}]/`).hostname
    return hostname.slice(1, -1).toLowerCase()
  } catch {
    return null
  }
}

/**
 * The dotted IPv4 address inside an IPv4-mapped IPv6 address, or null.
 *
 * The URL parser writes the mapped part in hex (::ffff:c0a8:105), so a dotted
 * pattern would never see it.
 */
function mappedIpv4(canonical) {
  const mapped = /^::ffff:([0-9a-f]{1,4}):([0-9a-f]{1,4})$/.exec(canonical)
  if (!mapped) return null
  const high = parseInt(mapped[1], 16)
  const low = parseInt(mapped[2], 16)
  return [high >> 8, high & 0xff, low >> 8, low & 0xff].join('.')
}

function classifyIpv6(canonical) {
  if (canonical === '::1') return 'loopback'
  if (canonical === '::') return 'unspecified'
  const first = parseInt(canonical.split(':')[0] || '0', 16)
  if ((first & 0xff00) === 0xff00) return 'multicast'
  if ((first & 0xffc0) === 0xfe80) return 'link-local'
  // Tailscale's IPv6 range is specific to it, unlike the shared IPv4 range.
  if (canonical.startsWith('fd7a:115c:a1e0:')) return 'tailscale'
  if ((first & 0xfe00) === 0xfc00) return 'lan'
  if (canonical.startsWith('2001:db8:')) return 'documentation'
  return 'public'
}

/**
 * Remembers which label each address received, so one address reads the same
 * everywhere it appears within one piece of shared text.
 */
export class NetworkAddressBook {
  constructor() {
    this.labels = new Map()
    this.counters = new Map()
  }

  /**
   * Number past any labels already present in `text`.
   *
   * Nova redacts a report before posting it and the host redacts again on export,
   * so text arriving here may already say [lan-1]. Starting this book at 1 as well
   * would give a different address that same label, and a reader would take two
   * devices for one.
   */
  reserveExistingLabels(text) {
    const scanner = new RegExp(LABEL_SOURCE, 'g')
    let match
    while ((match = scanner.exec(String(text || ''))) !== null) {
      const [, kind, index] = match
      this.counters.set(kind, Math.max(this.counters.get(kind) || 0, Number(index)))
    }
  }

  labelFor(key, kind) {
    const existing = this.labels.get(key)
    if (existing) return existing
    const next = (this.counters.get(kind) || 0) + 1
    this.counters.set(kind, next)
    const label = `[${kind}-${next}]`
    this.labels.set(key, label)
    return label
  }
}

/**
 * Replace every network address in `text` with a label that keeps its kind.
 *
 * Idempotent: a label contains no address, so a second pass leaves it alone.
 */
export function pseudonymizeNetworkAddresses(text, book = new NetworkAddressBook()) {
  const source = String(text ?? '')
  if (!source) return source
  book.reserveExistingLabels(source)

  const withIpv6 = source.replace(new RegExp(IPV6_CANDIDATE_SOURCE, 'g'), (whole, open, candidate, zone, close) => {
    // Brackets only belong to the address when they come as a pair.
    const bracketed = Boolean(open) && Boolean(close)
    const canonical = canonicalIpv6(candidate)
    if (!canonical) return whole
    // An IPv4-mapped address is that IPv4 address, so it gets the same kind and
    // the same label as the dotted form elsewhere in the text.
    const ipv4 = mappedIpv4(canonical)
    const kind = ipv4 ? classifyIpv4(ipv4) : classifyIpv6(canonical)
    if (KEPT_KINDS.has(kind)) return whole
    const label = book.labelFor(ipv4 || canonical, kind)
    if (bracketed) return label
    return `${open || ''}${label}${close || ''}`
  })

  return withIpv6.replace(new RegExp(IPV4_IN_TEXT_SOURCE, 'g'), (address) => {
    const kind = classifyIpv4(address)
    if (KEPT_KINDS.has(kind)) return address
    return book.labelFor(address, kind)
  })
}
