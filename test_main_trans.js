const express = require('express')
const app = express()

const PORT = 8080

app.use('/files', express.static('files'))
app.listen(PORT, '10.42.0.1', () => {
  console.log(`listening 10.42.0.1:${PORT}, serving files in /files`)
})
