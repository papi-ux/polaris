import { readFileSync } from 'node:fs'
import { join } from 'node:path'

describe('pairing network trust navigation', () => {
  it('links Configure Network Trust directly to encryption and trust settings', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/PinView.vue'), 'utf8')

    expect(source).toContain('href="#/config#encryption_and_trust"')
  })

  it('anchors the Network encryption and trust section for deep links', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/configs/tabs/Network.vue'), 'utf8')

    expect(source).toContain('id="encryption_and_trust"')
  })

  it('uses generic documentation-style trusted subnet examples', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/configs/tabs/Network.vue'), 'utf8')

    expect(source).toContain('fd00:1234:5678::/64')
    expect(source).not.toContain('fdeb:c779:7c30:dedf::/64')
  })
})
