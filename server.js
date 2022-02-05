const express = require('express')
const app = express()

app.use('/files', express.static('files'))

app.listen(80, "10.42.0.1", () => {
    console.log("hello")    
})
