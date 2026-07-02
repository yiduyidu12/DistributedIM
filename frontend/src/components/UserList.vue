<script setup lang="ts">
import { onMounted, onUnmounted } from 'vue'
import { useUserStore } from '@/stores/user'
import { useWebSocketStore } from '@/stores/websocket'

const emit = defineEmits<{
  (e: 'selectUser', username: string): void
}>()

const userStore = useUserStore()
const wsStore = useWebSocketStore()

function handleMessage(event: MessageEvent) {
  const data = JSON.parse(event.data)
  if (data.type === 'who') {
    userStore.setOnlineUsers(data.users)
  }
}

function fetchOnlineUsers() {
  wsStore.send('WHO')
}

onMounted(() => {
  wsStore.ws?.addEventListener('message', handleMessage)
  fetchOnlineUsers()
  setInterval(fetchOnlineUsers, 30000)
})

onUnmounted(() => {
  wsStore.ws?.removeEventListener('message', handleMessage)
})
</script>

<template>
  <div class="user-list-container">
    <h3>在线用户</h3>
    <div class="users">
      <div
        v-for="user in userStore.onlineUsers"
        :key="user.username"
        class="user-item"
        :class="{ 'active': user.online }"
        @click="emit('selectUser', user.username)"
      >
        <span class="status-dot"></span>
        <span class="username">{{ user.username }}</span>
      </div>
      <div v-if="userStore.onlineUsers.length === 0" class="empty">
        暂无在线用户
      </div>
    </div>
  </div>
</template>

<style scoped>
.user-list-container {
  padding: 16px;
  background: white;
  border-right: 1px solid #eee;
  width: 240px;
}

.user-list-container h3 {
  margin: 0 0 16px 0;
  color: #333;
  font-size: 16px;
}

.users {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.user-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 10px;
  border-radius: 8px;
  cursor: pointer;
  transition: background 0.2s;
}

.user-item:hover {
  background: #f5f5f5;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #10b981;
}

.user-item:not(.active) .status-dot {
  background: #9ca3af;
}

.username {
  color: #333;
  font-size: 14px;
}

.empty {
  color: #9ca3af;
  text-align: center;
  padding: 20px;
}
</style>