# Network trust and shared address space

Polaris treats `100.64.0.0/10` as WAN for access restrictions and stream encryption.
[RFC 6598](https://www.rfc-editor.org/rfc/rfc6598.html) reserves this range as
Shared Address Space. A numeric address in it does not establish that a peer is
on a trusted LAN or tunnel. IPv4-mapped IPv6 addresses follow the same policy.

This applies to VPN peers using that IPv4 range, including Tailscale peers.
Existing paired certificates remain paired. The behavior changes at these gates:

- A Web UI configured with `origin_web_ui_allowed = lan` refuses those peers.
  Credentials and paired identity do not bypass that origin restriction.
- Browser Stream remains LAN-only and refuses those peers.
- Native streaming selects `wan_encryption_mode`, including its mandatory
  encryption checks at launch, resume and RTSP negotiation. The default WAN
  mode is opportunistic; this change does not force mandatory encryption.
- Diagnostics still label the range `cgnat`, including both range endpoints.

`trusted_subnets` and `trusted_subnet_auto_pairing` are a separate, explicit
pairing opt-in. A matching configured CGNAT subnet can still participate in
Trusted Pair, with the existing client opt-in and authorization rules. Adding a
pairing subnet does not change the origin class or encryption mode. Do not add
the entire shared range merely because a VPN uses addresses within it.

There is no verified-route VPN exception in this address-only policy. A future
exception would need an explicit trust contract and verified route or interface
provenance; a hostname or interface-name prefix would not establish it.

Private IPv4 LAN, loopback, link-local and IPv6 ULA classifications are unchanged.
In particular, a VPN's ULA IPv6 address still follows the existing ULA policy;
this change does not reclassify every VPN transport.
