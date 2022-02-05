const net = require('net')
const express = require('express')
const app = express()

const tcp = net.createServer(socket => {
  console.log(`client connected, local: ${socket.localAddress}, remote: ${socket.remoteAddress}`)

  socket.on('data', data => {
    console.log(data.toString())
    const msg = JSON.stringify({cmd: "ota",url: "http://10.42.0.1/files/roadhill.bin"}) + '\n'
    console.log(msg)
    socket.write(msg)
  })
})

tcp.listen(8080, '10.42.0.1', () => {
  console.log('tcp server started, listening 10.42.0.1 @ 8080')
})

app.use('/files', express.static('files'))

app.listen(80, '10.42.0.1', () => {
  console.log('http server started, listening 10.42.0.1 @80, files located in /files')
})
