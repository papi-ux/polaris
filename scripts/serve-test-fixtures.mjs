#!/usr/bin/env node

import { createServer } from 'node:http'
import { readFile, stat } from 'node:fs/promises'
import { extname, resolve, sep } from 'node:path'
import process from 'node:process'

const root = resolve('tests/fixtures/http')
const rootPrefix = `${root}${sep}`
const portFlag = process.argv.indexOf('--port')
const port = Number(portFlag >= 0 ? process.argv[portFlag + 1] : (process.env.PORT || 3000))

if (!Number.isInteger(port) || port < 0 || port > 65535) {
  throw new Error(`invalid port: ${port}`)
}

const contentTypes = {
  '.css': 'text/css; charset=utf-8',
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.txt': 'text/plain; charset=utf-8',
}

const server = createServer(async (request, response) => {
  try {
    const pathname = decodeURIComponent(new URL(request.url || '/', 'http://127.0.0.1').pathname)
    let file = resolve(root, `.${pathname}`)
    if (file !== root && !file.startsWith(rootPrefix)) {
      response.writeHead(403).end('Forbidden\n')
      return
    }

    if ((await stat(file)).isDirectory()) {
      file = resolve(file, 'index.html')
    }

    const body = await readFile(file)
    response.writeHead(200, {
      'Content-Type': contentTypes[extname(file)] || 'application/octet-stream',
      'Content-Length': body.length,
    })
    response.end(body)
  } catch (error) {
    const status = error?.code === 'ENOENT' ? 404 : 500
    response.writeHead(status).end(status === 404 ? 'Not Found\n' : 'Internal Server Error\n')
  }
})

server.listen(port, '127.0.0.1', () => {
  const address = server.address()
  console.log(`Fixture server listening at http://127.0.0.1:${address.port}`)
})
