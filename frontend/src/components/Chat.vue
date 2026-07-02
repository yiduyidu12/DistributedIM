<script setup lang="ts">
import { ref, watch, nextTick } from 'vue'
import { useMessageStore } from '@/stores/message'
import { useWebSocketStore } from '@/stores/websocket'
import { useUserStore } from '@/stores/user'

const props = defineProps<{
  targetUser: string
}>()

const messageStore = useMessageStore()
const wsStore = useWebSocketStore()
const userStore = useUserStore()

const newMessage = ref('')
const messages = ref<{ from: string; content: string; timestamp: number }[]>([])

watch(() => props.targetUser, () => {
  loadMessages()
}, { immediate: true })

function loadMessages() {
  const userMessages = messageStore.getMessagesByUser(props.targetUser)
  messages.value = userMessages.map(m => ({
    from: m.from,
    content: m.content,
    timestamp: m.timestamp
  }))
}

async function sendMessage() {
  if (!newMessage.value.trim()) return

  const msgId = Date.now().toString()
  
  wsStore.send(JSON.stringify({
    type: 'send',
    to: props.targetUser,
    msg: newMessage.value.trim(),
    msg_id: msgId
  }))

  messages.value.push({
    from: userStore.currentUser?.username || 'me',
    content: newMessage.value.trim(),
    timestamp: Date.now()
  })

  newMessage.value = ''
  await nextTick()
  scrollToBottom()
}

function scrollToBottom() {
  const container = document.querySelector('.messages-container')
  if (container) {
    container.scrollTop = container.scrollHeight
  }
}

function formatTime(timestamp: number): string {
  const date = new Date(timestamp * 1000)
  return `${date.getHours().toString().padStart(2, '0')}:${date.getMinutes().toString().padStart(2, '0')}`
}
</script>

<template>
  <div class="chat-container">
    <div class="chat-header">
      <h2>{{ targetUser }}</h2>
    </div>
    <div class="messages-container">
      <div
        v-for="(msg, index) in messages"
        :key="index"
        class="message"
        :class="{ 'sent': msg.from === userStore.currentUser?.username }"
      >
        <div class="message-content">{{ msg.content }}</div>
        <div class="message-time">{{ formatTime(msg.timestamp) }}</div>
      </div>
    </div>
    <div class="input-container">
      <input
        v-model="newMessage"
        type="text"
        placeholder="输入消息..."
        @keyup.enter="sendMessage"
      />
      <button @click="sendMessage">发送</button>
    </div>
  </div>
</template>

<style scoped>
.chat-container {
  flex: 1;
  display: flex;
  flex-direction: column;
  height: 100%;
  background: #f5f5f5;
}

.chat-header {
  background: white;
  padding: 16px;
  border-bottom: 1px solid #eee;
}

.chat-header h2 {
  margin: 0;
  color: #333;
  font-size: 18px;
}

.messages-container {
  flex: 1;
  overflow-y: auto;
  padding: 16px;
}

.message {
  max-width: 70%;
  margin-bottom: 12px;
  padding: 10px 14px;
  border-radius: 16px;
}

.message.sent {
  background: #667eea;
  color: white;
  margin-left: auto;
  border-bottom-right-radius: 4px;
}

.message:not(.sent) {
  background: white;
  color: #333;
  border-bottom-left-radius: 4px;
}

.message-content {
  font-size: 15px;
  line-height: 1.4;
}

.message-time {
  font-size: 11px;
  margin-top: 4px;
  opacity: 0.7;
}

.input-container {
  display: flex;
  gap: 8px;
  padding: 16px;
  background: white;
  border-top: 1px solid #eee;
}

.input-container input {
  flex: 1;
  padding: 12px 16px;
  border: 1px solid #eee;
  border-radius: 24px;
  font-size: 15px;
}

.input-container input:focus {
  outline: none;
  border-color: #667eea;
}

.input-container button {
  padding: 12px 24px;
  background: #667eea;
  color: white;
  border: none;
  border-radius: 24px;
  cursor: pointer;
}

.input-container button:hover {
  background: #5a6fd6;
}
</style>