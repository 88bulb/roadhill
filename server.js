const fs = require('fs')
const net = require('net')
const express = require('express')
const app = express()

fs.cp('build/roadhill.bin', 'files/roadhill.bin', err => {
  if (err) return

  fs.readFile('files/roadhill.bin', (err, content) => {
    if (err) return

    const sha256 = content.slice(-32).toString('hex')
    console.log(`roadhill.bin sha256: ${sha256}`)

    const tcp = net.createServer(socket => {
      console.log('client connected,' +
                  `local: ${socket.localAddress}, ` +
                  `remote: ${socket.remoteAddress}`)

      socket.on('data', data => {
        const text = data.toString()
        try {
          const msg = JSON.parse(text)

          console.log('incoming <-', msg)

          if (msg.type === 'DEVICE_INFO') {
            if (msg.firmware.sha256 !== sha256) {
              const outgoing = JSON.stringify({
                cmd: 'OTA',
                url: 'http://10.42.0.1/files/roadhill.bin'
              }) + '\n'
              console.log(`outgoing -> ${outgoing}`)
              socket.write(outgoing)
            } else {
              const outgoing = JSON.stringify({
                cmd: 'PLAY',
                version: '1.0',
                // tracks_url: 'http://shinuojbs-1306173101.cos.ap-shanghai.myqcloud.com/sgd_audio/8b76c5faf3fd66b66d5a36e6ffe9e7bb.mp3',
                tracks_url: 'http://10.42.0.1/files/album000001',
                // tracks_url: 'http://shinuojbs-1306173101.cos.ap-shanghai.myqcloud.com/sgd_audio',
                tracks: [
                  {
                    name: '8b76c5faf3fd66b66d5a36e6ffe9e7bb.mp3',
                    size: 6817965,
                    position: 0,
                    begin: 0,
                    end: -1,
                    chan: 0
                  },
                  {
                    name: '894d204819a508c893912166d8746338.mp3',
                    size: 1009228,
                    position: 5000,
                    begin: 0,
                    end: -1,
                    chan: 1
                  }
                ],
                blinks: [
                  {
                    time: 0,
                    mask: 'ffff',
                    code: '100300000003c680c6f0fa33f0fafa'
                  },
                  {
                    time: 1000,
                    mask: 'ffff',
                    code: '100300000003c680c6f0fa33f0fafa'
                  },
                  {
                    time: 3000,
                    mask: 'ffff',
                    code: '100300000003c680c6f0fa33f0fafa'
                  }
                ],
                start: 'immediate'
              }) + '\n'
              console.log(`outgoing -> ${outgoing}`)
              socket.write(outgoing)
            }
          }
        } catch (e) {

        }
      })
    })

    tcp.listen(8080, '10.42.0.1', () => {
      console.log('tcp server started, ' +
                  'listening 10.42.0.1 @ 8080')
    })

    app.use('/files', express.static('files'))
    app.listen(80, '10.42.0.1', () => {
      console.log('http server started, ' +
                  'listening 10.42.0.1 @ 80, files located in /files')
    })
  })
})
