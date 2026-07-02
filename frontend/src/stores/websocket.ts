import { defineStore } from 'pinia'
import { ref } from 'vue'

export const useWebSocketStore = defineStore('websocket', () => {
  const ws = ref<WebSocket | null>(null)
  const connected = ref(false)
  const reconnecting = ref(false)

  function connect(url: string) {
    return new Promise<void>((resolve, reject) => {
      if (ws.value) {
        ws.value.close()
      }

      ws.value = new WebSocket(url)

      ws.value.onopen = () => {
        connected.value = true
        reconnecting.value = false
        resolve()
      }

      ws.value.onclose = () => {
        connected.value = false
        if (!reconnecting.value) {
          setTimeout(() => {
            reconnecting.value = true
            connect(url)
          }, 3000)
        }
      }

      ws.value.onerror = (error) => {
        connected.value = false
        reject(error)
      }
    })
  }

  function send(message: string) {
    if (ws.value && connected.value) {
      ws.value.send(message)
    }
  }

  function close() {
    if (ws.value) {
      ws.value.close()
      ws.value = null
    }
    connected.value = false
  }

  return {
    ws,
    connected,
    reconnecting,
    connect,
    send,
    close
  }
})