// Shape cases for the diagnostics redactor.
//
// Every defect found in this file so far has been the same failure: the scan not
// seeing a name that is there, or not agreeing with a human about where that
// name's value ends. None of them were found by reading the code. They were
// found by running a shape nobody had written a case for.
//
// So these are organised by shape rather than by name. When adding to this file,
// prefer a case that violates an assumption every existing case shares over
// another instance of a shape already covered.
//
// Place beside diagnostics-export.js and adjust the import if it moves.
import { describe, expect, it } from 'vitest'
import { redactSensitiveText, REDACTED_VALUE } from './diagnostics-export.js'

const SECRET = 'abc123'
const leaks = (input) => redactSensitiveText(input).includes(SECRET)

describe('redactSensitiveText: value quoting', () => {
  it.each([
    ['double-quoted value', 'auth_token="abc123"'],
    ['single-quoted value', "api_key='abc123'"],
    ['yaml', 'api_key: "abc123"'],
    ['bracketed value', 'token=[abc123]'],
    ['bracketed value behind a prefix', 'Warning: token=[abc123]'],
    ['value containing the separator', 'api_key=a=b=c=abc123'],
    ['value containing a colon', 'auth_token=abc123:extra'],
    ['url as the value', 'api_key=https://x.test/?q=abc123'],
  ])('redacts %s', (_label, input) => {
    expect(leaks(input)).toBe(false)
  })
})

describe('redactSensitiveText: separator spacing', () => {
  it.each([
    ['spaces around equals', 'api_key  =  abc123'],
    ['tabs around equals', 'api_key\t=\tabc123'],
    ['colon with no space', 'api_key:abc123'],
  ])('redacts %s', (_label, input) => {
    expect(leaks(input)).toBe(false)
  })
})

// A closing delimiter used to sit between the name and its separator and hide
// the name from the scan. That is every JSON key there is, since JSON quotes
// them all.
describe('redactSensitiveText: delimiters between the name and its separator', () => {
  it.each([
    ['quoted key', '{"api_key": "abc123"}'],
    ['quoted key, bare value', '"api_key": abc123'],
    ['quoted key with equals', '"api_key"=abc123'],
    ['single-quoted key', "{'api_key': 'abc123'}"],
    ['parenthesised name', '(api_key): abc123'],
    ['bracketed name', '[api_key]: abc123'],
    ['nested objects', '{"a": {"b": {"api_key": "abc123"}}}'],
    ['sibling keys', '{"a": "x", "api_key": "abc123"}'],
    ['realistic config shape', '{"root": {"api_key": "abc123", "port": 47990}}'],
  ])('redacts %s', (_label, input) => {
    expect(leaks(input)).toBe(false)
  })
})

// The inverse failure: the name is seen, but the value is taken to end at the
// nested name rather than at the end of the object, so only the opening
// fragment is replaced and the secret survives one level down. Worse than a
// plain miss, because "auth": [redacted] reads as if the subtree was handled.
describe('redactSensitiveText: a sensitive name whose value is a structure', () => {
  it.each([
    ['object value', '{"auth": {"api_key": "abc123"}}'],
    ['object value, nested', '{"cfg": {"auth": {"api_key": "abc123"}}}'],
    ['object value, bare names', '{auth: {api_key: "abc123"}}'],
    ['object value, no braces around the pair', 'auth: {api_key: "abc123"}'],
    ['object value under a different name', '{"secret": {"inner": "abc123"}}'],
    ['array value', '{"tokens": ["abc123", "x"]}'],
  ])('redacts %s', (_label, input) => {
    expect(leaks(input)).toBe(false)
  })
})

// An innocent pair must not consume a sensitive one that follows it.
describe('redactSensitiveText: ordering', () => {
  it.each([
    ['innocent then sensitive', 'keyboard=us auth_token=abc123'],
    ['two innocent then sensitive', 'monkey=banana keyboard=us api_key=abc123'],
    ['sensitive between innocents', 'keyboard=us api_key=abc123 monkey=banana'],
    ['innocent-sounding name first', 'keyName=capture_path client_secret=abc123'],
    ['repeated on one line', 'first=ok auth_token=abc123 second=ok apikey=abc123'],
  ])('redacts %s', (_label, input) => {
    expect(leaks(input)).toBe(false)
  })
})

describe('redactSensitiveText: log framing', () => {
  it.each([
    ['timestamped', '[2026-08-17 20:31:02.123]: Info: config: api_key = abc123'],
    ['syslog style', 'polaris[591466]: Warning: request failed apiKey=abc123'],
    ['java stack frame', 'at com.papi.nova.Foo.bar(Foo.kt:42): client_secret=abc123'],
    ['exception chain', 'Caused by: java.lang.RuntimeException: db-password=abc123'],
    ['http request line', 'GET /api/config?apikey=abc123 HTTP/1.1'],
    ['header', 'curl -H "Authorization: Bearer abc123" https://host/x'],
    ['set-cookie', 'set-cookie: session=abc123; Path=/'],
    ['logfmt', 'msg="auth failed" auth_token=abc123 status=401'],
    ['multi-line block', 'Info: start\nWarning: auth_token=abc123\nInfo: done'],
    ['crlf block', 'Info: start\r\nWarning: api_key=abc123\r\nInfo: end'],
  ])('redacts %s', (_label, input) => {
    expect(leaks(input)).toBe(false)
  })
})

// Real Polaris journal lines. Redaction that eats these makes the bundle
// useless, which is the failure mode nobody reports because the bundle still
// looks fine.
describe('redactSensitiveText: diagnostics survive byte for byte', () => {
  it.each([
    'Info: wlr: capture_transport=dmabuf frame_residency=gpu frame_format=bgra8',
    'Info: wlr: screencopy capture pacing=source_driven private_compositor=true',
    'Info: session_runtime: path=headless_dongle backend=portal requested_headless=true',
    'Info: display_topology: auto-selected primary_output=[DP-2]',
    'Info: display_topology: requested mode [1920x1080@60Hz] on output [HDMI-A-2]',
    'Info: kwingrab: stream ready node=161 serial=8154 output=HDMI-A-2 3840x2160',
    'Info: Session stream mode override requested: [headless_stream]',
    'Info: config: linux_stream_mode = headless_dongle',
    'Info: capture_path=dmabuf',
    'level: info',
    'session_overridable=false',
    'encode_time_ms=3.28',
    'keyboard=us monkey=banana turnkey=yes passwordless=true',
    'keyName=capture_path',
    // quoted innocent keys: recognising quoted names must not start eating these
    '{"level": "info"}',
    '{"keyName": "capture_path"}',
    '{"capture_path": "dmabuf", "frame_residency": "gpu"}',
    '{"keyboard": "us", "monkey": "banana"}',
  ])('leaves %s unchanged', (line) => {
    expect(redactSensitiveText(line)).toBe(line)
  })

  it('leaves the redaction marker itself alone when it is an innocent value', () => {
    const line = `capture_path=${REDACTED_VALUE}`
    expect(redactSensitiveText(line)).toBe(line)
  })
})

describe('redactSensitiveText: redacting an already-redacted document is a no-op', () => {
  it.each([
    'auth_token=abc123',
    'Warning: token=[abc123]',
    '{"api_key": "abc123"}',
    `auth_token=${REDACTED_VALUE} api_key=abc123`,
    `api_key=abc123 auth_token=${REDACTED_VALUE}`,
    'Info: start\nWarning: auth_token=abc123\nInfo: done',
  ])('is stable across three passes: %s', (input) => {
    const once = redactSensitiveText(input)
    const twice = redactSensitiveText(once)
    expect(twice).toBe(once)
    expect(redactSensitiveText(twice)).toBe(once)
  })
})

describe('redactSensitiveText: degenerate input', () => {
  it.each([
    'api_key=', 'api_key', '=abc123', '', '   ', 'a'.repeat(5000),
    '{"api_key":', 'token=[', 'token=]', '[[[[[[', 'api_key=[[[[',
  ])('does not throw on %j', (input) => {
    expect(() => redactSensitiveText(input)).not.toThrow()
  })
})

describe('redactSensitiveText: cost', () => {
  it('stays linear on a bundle-sized input and still catches a secret at the end', () => {
    const big = `${Array.from({ length: 20000 }, (_, i) =>
      `Info: frame=${i} capture_path=dmabuf keyboard=us encode_time_ms=3.28`).join('\n')
    }\nWarning: api_key=abc123\n`
    const started = Date.now()
    const out = redactSensitiveText(big)
    expect(Date.now() - started).toBeLessThan(3000)
    expect(out.includes(SECRET)).toBe(false)
  })
})
