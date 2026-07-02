<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'
import { useUserStore } from '@/stores/user'
import { useMessageStore } from '@/stores/message'
import { useWebSocketStore } from '@/stores/websocket'
import Login from '@/components/Login.vue'
import UserList from '@/components/UserList.vue'
import Chat from '@/components/Chat.vue'
import GroupList from '@/components/GroupList.vue'
import GroupChat from '@/components/GroupChat.vue'

const userStore = useUserStore()
const messageStore = useMessageStore()
const wsStore = useWebSocketStore()

const loggedIn = ref(false)
const selectedUser = ref<string | null>(null)
const selectedGroup = ref<{ groupId: string; groupName: string } | null>(null)
const showGroups = ref(false)

function handleLoggedIn() {
  loggedIn.value = true
}

function handleSelectUser(username: string) {
  selectedUser.value = username
  selectedGroup.value = null
  showGroups.value = false
}

function handleSelectGroup(groupId: string, groupName: string) {
  selectedGroup.value = { groupId, groupName }
  selectedUser.value = null
  showGroups.value = true
}

function handleMessage(event: MessageEvent) {
  const data = JSON.parse(event.data)
  
  if (data.type === 'chat' || data.type === 'send') {
    messageStore.addMessage({
      msgId: data.msg_id || Date.now().toString(),
      type: data.type,
      from: data.from,
      to: data.to,
      content: data.msg,
      timestamp: data.timestamp
    })
    
    if (selectedUser.value === data.from || selectedUser.value === data.to) {
      selectedUser.value = data.from
    }
  } else if (data.type === 'group_send') {
    messageStore.addMessage({
      msgId: data.msg_id || Date.now().toString(),
      type: data.type,
      from: data.from,
      groupId: data.group_id,
      content: data.msg,
      timestamp: data.timestamp
    })
    
    if (selectedGroup.value?.groupId === data.group_id) {
      selectedGroup.value = { groupId: data.group_id, groupName: selectedGroup.value.groupName }
    }
  }
}

function handleLogout() {
  wsStore.send(JSON.stringify({ type: 'logout' }))
  wsStore.close()
  userStore.clearCurrentUser()
  messageStore.clearMessages()
  loggedIn.value = false
  selectedUser.value = null
  selectedGroup.value = null
}

onMounted(() => {
  if (wsStore.ws) {
    wsStore.ws.addEventListener('message', handleMessage)
  }
  
  const interval = setInterval(() => {
    if (loggedIn.value && wsStore.connected) {
      wsStore.send(JSON.stringify({ type: 'ping' }))
    }
  }, 30000)
  
  onUnmounted(() => {
    clearInterval(interval)
    wsStore.ws?.removeEventListener('message', handleMessage)
    wsStore.close()
  })
})
</script>

<template>
  <div v-if="!loggedIn" class="app">
    <Login @logged-in="handleLoggedIn" />
  </div>
  
  <div v-else class="app-main">
    <div class="sidebar">
      <div class="sidebar-header">
        <div class="user-info">
          <div class="avatar">{{ userStore.currentUser?.username?.charAt(0) || 'U' }}</div>
          <div class="username">{{ userStore.currentUser?.username }}</div>
        </div>
        <button class="logout-btn" @click="handleLogout">退出</button>
      </div>
      
      <div class="sidebar-tabs">
        <button
          class="tab-btn"
          :class="{ active: !showGroups }"
          @click="showGroups = false"
        >
          👤 好友
        </button>
        <button
          class="tab-btn"
          :class="{ active: showGroups }"
          @click="showGroups = true"
        >
          👥 群组
        </button>
      </div>
      
      <UserList v-if="!showGroups" @select-user="handleSelectUser" />
      <GroupList v-else @select-group="handleSelectGroup" />
    </div>
    
    <div class="main-content">
      <div v-if="!selectedUser && !selectedGroup" class="empty-state">
        <div class="empty-icon">💬</div>
        <p>选择一个用户或群组开始聊天</p>
      </div>
      
      <Chat
        v-else-if="selectedUser"
        :target-user="selectedUser"
      />
      
      <GroupChat
        v-else-if="selectedGroup"
        :group-id="selectedGroup.groupId"
        :group-name="selectedGroup.groupName"
      />
    </div>
  </div>
</template>

<style>
* {
  margin: 0;
  padding: 0;
  box-sizing: border-box;
}

body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, Ubuntu, sans-serif;
}

.app {
  width: 100vw;
  height: 100vh;
}

.app-main {
  display: flex;
  height: 100vh;
}

.sidebar {
  display: flex;
  flex-direction: column;
  width: 280px;
  border-right: 1px solid #eee;
}

.sidebar-header {
  padding: 16px;
  background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  color: white;
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.user-info {
  display: flex;
  align-items: center;
  gap: 10px;
}

.avatar {
  width: 36px;
  height: 36px;
  border-radius: 50%;
  background: rgba(255, 255, 255, 0.3);
  display: flex;
  align-items: center;
  justify-content: center;
  font-weight: 600;
}

.logout-btn {
  padding: 6px 12px;
  background: rgba(255, 255, 255, 0.2);
  color: white;
  border: 1px solid rgba(255, 255, 255, 0.3);
  border-radius: 4px;
  cursor: pointer;
  font-size: 12px;
}

.logout-btn:hover {
  background: rgba(255, 255, 255, 0.3);
}

.sidebar-tabs {
  display: flex;
  border-bottom: 1px solid #eee;
}

.tab-btn {
  flex: 1;
  padding: 12px;
  border: none;
  background: white;
  color: #666;
  cursor: pointer;
  font-size: 14px;
  border-bottom: 2px solid transparent;
}

.tab-btn.active {
  color: #667eea;
  border-bottom-color: #667eea;
}

.main-content {
  flex: 1;
  display: flex;
}

.empty-state {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  background: #f5f5f5;
}

.empty-icon {
  font-size: 64px;
  margin-bottom: 16px;
}

.empty-state p {
  color: #9ca3af;
  font-size: 16px;
}
</style>