<script setup lang="ts">
import { onMounted, onUnmounted } from 'vue'
import { useGroupStore } from '@/stores/group'
import { useWebSocketStore } from '@/stores/websocket'

const emit = defineEmits<{
  (e: 'selectGroup', groupId: string, groupName: string): void
}>()

const groupStore = useGroupStore()
const wsStore = useWebSocketStore()

function handleMessage(event: MessageEvent) {
  const data = JSON.parse(event.data)
  if (data.type === 'group_list') {
    const groups = data.groups || []
    groupStore.setGroups(groups.map((g: { group_id: string; group_name: string; creator: string; members: string[] }) => ({
      groupId: g.group_id,
      groupName: g.group_name,
      creator: g.creator,
      members: g.members,
      memberCount: g.members?.length || 0
    })))
  }
}

function fetchGroups() {
  wsStore.send(JSON.stringify({ type: 'group_list' }))
}

onMounted(() => {
  wsStore.ws?.addEventListener('message', handleMessage)
  fetchGroups()
})

onUnmounted(() => {
  wsStore.ws?.removeEventListener('message', handleMessage)
})
</script>

<template>
  <div class="group-list-container">
    <h3>群组</h3>
    <div class="groups">
      <div
        v-for="group in groupStore.groups"
        :key="group.groupId"
        class="group-item"
        @click="emit('selectGroup', group.groupId, group.groupName)"
      >
        <div class="group-icon">👥</div>
        <div class="group-info">
          <div class="group-name">{{ group.groupName }}</div>
          <div class="group-count">{{ group.memberCount }} 成员</div>
        </div>
      </div>
      <div v-if="groupStore.groups.length === 0" class="empty">
        暂无群组
      </div>
    </div>
  </div>
</template>

<style scoped>
.group-list-container {
  padding: 16px;
  background: white;
  border-right: 1px solid #eee;
  width: 240px;
}

.group-list-container h3 {
  margin: 0 0 16px 0;
  color: #333;
  font-size: 16px;
}

.groups {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.group-item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 10px;
  border-radius: 8px;
  cursor: pointer;
  transition: background 0.2s;
}

.group-item:hover {
  background: #f5f5f5;
}

.group-icon {
  font-size: 20px;
}

.group-info {
  display: flex;
  flex-direction: column;
}

.group-name {
  color: #333;
  font-size: 14px;
  font-weight: 500;
}

.group-count {
  color: #9ca3af;
  font-size: 12px;
}

.empty {
  color: #9ca3af;
  text-align: center;
  padding: 20px;
}
</style>