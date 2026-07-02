<script setup lang="ts">
import { ref } from 'vue'
import { useUserStore } from '@/stores/user'
import { useWebSocketStore } from '@/stores/websocket'

const emit = defineEmits<{
  (e: 'loggedIn'): void
}>()

const username = ref('')
const userStore = useUserStore()
const wsStore = useWebSocketStore()

async function handleLogin() {
  if (!username.value.trim()) return

  await wsStore.connect('ws://localhost:8888/ws')
  
  wsStore.send(JSON.stringify({
    type: 'login',
    username: username.value.trim()
  }))

  wsStore.ws?.onmessage = (event) => {
    const response = JSON.parse(event.data)
    if (response.type === 'login' && response.status === 'ok') {
      userStore.setCurrentUser({
        username: username.value.trim(),
        online: true,
        gatewayId: response.gateway_id
      })
      emit('loggedIn')
    }
  }
}
</script>

<template>
  <div class="login-container">
    <div class="login-card">
      <h1>DistributedIM</h1>
      <p class="subtitle">分布式即时通讯系统</p>
      <div class="input-group">
        <input
          v-model="username"
          type="text"
          placeholder="请输入用户名"
          @keyup.enter="handleLogin"
        />
      </div>
      <button class="login-btn" @click="handleLogin">
        登录
      </button>
      <div v-if="wsStore.reconnecting" class="status">
        重连中...
      </div>
    </div>
  </div>
</template>

<style scoped>
.login-container {
  display: flex;
  justify-content: center;
  align-items: center;
  min-height: 100vh;
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
}

.login-card {
  background: white;
  padding: 40px;
  border-radius: 16px;
  box-shadow: 0 10px 40px rgba(0, 0, 0, 0.2);
  width: 400px;
  text-align: center;
}

.login-card h1 {
  margin: 0 0 8px 0;
  color: #333;
  font-size: 28px;
}

.subtitle {
  color: #666;
  margin-bottom: 30px;
}

.input-group input {
  width: 100%;
  padding: 14px 16px;
  border: 2px solid #eee;
  border-radius: 8px;
  font-size: 16px;
  transition: border-color 0.3s;
  box-sizing: border-box;
}

.input-group input:focus {
  outline: none;
  border-color: #667eea;
}

.login-btn {
  width: 100%;
  padding: 14px;
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  color: white;
  border: none;
  border-radius: 8px;
  font-size: 16px;
  cursor: pointer;
  margin-top: 20px;
  transition: opacity 0.3s;
}

.login-btn:hover {
  opacity: 0.9;
}

.status {
  margin-top: 16px;
  color: #667eea;
}
</style>