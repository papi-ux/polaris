// Trusted subnets as the host stores them: Settings writes a comma list, hand-written files use
// a JSON or bracketed list, and entries are sometimes quoted. Every form is read; the comma list
// Settings writes is what gets saved.
export function parseTrustedSubnets(value) {
  let entries = []
  if (Array.isArray(value)) {
    entries = value
  } else if (typeof value === 'string' && value.trim()) {
    const text = value.trim()
    try {
      const parsed = JSON.parse(text)
      entries = Array.isArray(parsed) ? parsed : [text]
    } catch {
      entries = text.replace(/^\[/, '').replace(/\]$/, '').split(/[\n,]+/)
    }
  }
  return entries
    .map((entry) => String(entry ?? '').trim().replace(/^(['"])(.*)\1$/, '$2').trim())
    .filter(Boolean)
}

export function serializeTrustedSubnets(entries) {
  return entries.join(',')
}

function validIpv6(address) {
  const halves = address.split('::')
  if (halves.length > 2) return false
  const groups = halves.map((half) => (half === '' ? [] : half.split(':')))
  if (groups.some((list) => list.some((group) => !/^[0-9a-f]{1,4}$/.test(group)))) return false
  const count = groups.reduce((total, list) => total + list.length, 0)
  return halves.length === 2 ? count < 8 : count === 8
}

// A network the pairing check accepts, in the form it is stored: { cidr } or { error }.
// IPv4 host bits are cleared, so 192.168.1.20/24 is saved as the network it names. Very broad
// networks are refused, because trusting one lets every device on it pair without a PIN.
export function normalizeCidr(input) {
  const text = String(input ?? '').trim()
  const v4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\/(\d{1,2})$/.exec(text)
  if (v4) {
    const octets = v4.slice(1, 5).map(Number)
    const prefix = Number(v4[5])
    if (octets.some((octet) => octet > 255) || prefix > 32) return { error: 'invalid' }
    if (prefix < 8) return { error: 'too_broad' }
    const address = octets.reduce((total, octet) => total * 256 + octet, 0)
    const size = 2 ** (32 - prefix)
    const network = Math.floor(address / size) * size
    const dotted = [24, 16, 8, 0].map((shift) => Math.floor(network / 2 ** shift) % 256).join('.')
    return { cidr: `${dotted}/${prefix}` }
  }
  const v6 = /^([0-9a-fA-F:]+)\/(\d{1,3})$/.exec(text)
  if (v6) {
    const address = v6[1].toLowerCase()
    const prefix = Number(v6[2])
    if (prefix > 128 || !validIpv6(address)) return { error: 'invalid' }
    if (prefix < 32) return { error: 'too_broad' }
    return { cidr: `${address}/${prefix}` }
  }
  return { error: 'invalid' }
}

function comparable(entry) {
  const text = String(entry ?? '').trim().replace(/^(['"])(.*)\1$/, '$2').trim()
  return normalizeCidr(text).cidr || text.toLowerCase()
}

export function trustsSubnet(entries, cidr) {
  const wanted = comparable(cidr)
  return entries.some((entry) => comparable(entry) === wanted)
}

// The saved list with one network added; existing entries are kept as they were written.
export function withTrustedSubnet(entries, cidr) {
  return trustsSubnet(entries, cidr) ? [...entries] : [...entries, cidr]
}
