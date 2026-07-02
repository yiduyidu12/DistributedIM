import { defineStore } from 'pinia'
import { ref } from 'vue'
import type { Group } from '@/types'

export const useGroupStore = defineStore('group', () => {
  const groups = ref<Group[]>([])

  function setGroups(newGroups: Group[]) {
    groups.value = newGroups
  }

  function addGroup(group: Group) {
    const existing = groups.value.find(g => g.groupId === group.groupId)
    if (!existing) {
      groups.value.push(group)
    }
  }

  function removeGroup(groupId: string) {
    groups.value = groups.value.filter(g => g.groupId !== groupId)
  }

  function updateGroupMembers(groupId: string, members: string[]) {
    const group = groups.value.find(g => g.groupId === groupId)
    if (group) {
      group.members = members
      group.memberCount = members.length
    }
  }

  function getGroup(groupId: string): Group | undefined {
    return groups.value.find(g => g.groupId === groupId)
  }

  return {
    groups,
    setGroups,
    addGroup,
    removeGroup,
    updateGroupMembers,
    getGroup
  }
})