<template>
  <t-popup expand-animation placement="bottom-right" trigger="click" @visible-change="onOpen">
    <template #content>
      <div class="header-msg">
        <div class="header-msg-top">
          <p>告警通知</p>
          <t-button v-if="alerts.length > 0" class="clear-btn" variant="text" theme="primary" @click="goAlerts">
            查看全部
          </t-button>
        </div>
        <t-list v-if="alerts.length > 0" class="narrow-scrollbar" :split="false">
          <t-list-item v-for="(item, index) in alerts" :key="index">
            <div>
              <p class="msg-content">{{ item.title }}</p>
              <p class="msg-type">{{ kindText(item.kind) }} · {{ item.agent_id }}</p>
            </div>
            <p class="msg-time">{{ formatTime(item.created_at) }}</p>
            <template #action>
              <t-button size="small" variant="outline" @click="resolve(item)">标为已读</t-button>
            </template>
          </t-list-item>
        </t-list>
        <div v-else class="empty-msg">
          <t-icon name="check-circle" size="32" style="color: #2ba471" />
          <p>暂无未读告警</p>
        </div>
      </div>
    </template>
    <div class="header-msg-trigger">
      <t-badge v-if="unread > 0" :count="unread" color="red">
        <t-icon name="notification" size="20" />
      </t-badge>
      <t-icon v-else name="notification" size="20" />
    </div>
  </t-popup>
</template>

<script setup lang="ts">
import { onMounted, onUnmounted, ref } from 'vue';
import { useRouter } from 'vue-router';
import dayjs from 'dayjs';

import { listAlerts, resolveAlert, type AlertItem } from '@/api/agent';

const router = useRouter();
const alerts = ref<AlertItem[]>([]);
const unread = ref(0);
let timer: number | undefined;

function kindText(k: string) {
  return (
    {
      offline: '离线',
      high_cpu: 'CPU 过高',
      high_mem: '内存过高',
      high_disk: '磁盘过高',
      net_spike: '流量异常',
      ssh_brute: 'SSH 爆破',
    }[k] || k
  );
}

function formatTime(ms: number) {
  const diff = Date.now() - ms;
  if (diff < 60000) return '刚刚';
  if (diff < 3600000) return Math.floor(diff / 60000) + ' 分钟前';
  if (diff < 86400000) return Math.floor(diff / 3600000) + ' 小时前';
  return dayjs(ms).format('MM-DD HH:mm');
}

async function fetchAlerts() {
  try {
    const list = await listAlerts(false);
    alerts.value = list.slice(0, 8);
    unread.value = list.length;
  } catch {
    /* 未登录或接口异常时静默 */
  }
}

async function resolve(item: AlertItem) {
  try {
    await resolveAlert(item.agent_id, item.kind);
    await fetchAlerts();
  } catch {
    /* ignore */
  }
}

function goAlerts() {
  router.push('/security/alerts');
}

function onOpen(v: boolean) {
  if (v) fetchAlerts();
}

onMounted(() => {
  fetchAlerts();
  timer = window.setInterval(fetchAlerts, 30000);
});
onUnmounted(() => window.clearInterval(timer));
</script>

<style lang="less" scoped>
.header-msg-trigger {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 40px;
  height: 40px;
  cursor: pointer;
  border-radius: 4px;

  &:hover {
    background: var(--td-bg-color-container-hover);
  }
}

.header-msg {
  width: 340px;
  max-height: 480px;
  overflow-y: auto;
}

.header-msg-top {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 12px 16px;
  border-bottom: 1px solid var(--td-component-stroke);

  p {
    font-size: 14px;
    font-weight: 600;
    margin: 0;
  }
}

.msg-content {
  font-size: 13px;
  margin: 0 0 4px;
  color: var(--td-text-color-primary);
}

.msg-type {
  font-size: 11px;
  color: var(--td-text-color-placeholder);
  margin: 0;
}

.msg-time {
  font-size: 11px;
  color: var(--td-text-color-placeholder);
  white-space: nowrap;
}

.empty-msg {
  padding: 40px 16px;
  text-align: center;
  color: var(--td-text-color-placeholder);
  font-size: 13px;

  p {
    margin: 8px 0 0;
  }
}
</style>
