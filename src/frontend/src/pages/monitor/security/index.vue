<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>安全审计</span>
          <t-select v-model="currentAgent" :options="agentOptions" placeholder="选择主机" style="width: 260px" :loading="loadingAgents" @change="fetchAudit" />
        </t-space>
      </template>
      <template #actions>
        <t-button theme="primary" :loading="loading" @click="fetchAudit">重新扫描</t-button>
      </template>

      <t-loading :loading="loading">
        <template v-if="audit">
          <!-- 检查结果 -->
          <h3 class="sec-title">合规检查</h3>
          <t-row :gutter="12" style="margin-bottom: 20px">
            <t-col :span="8" v-for="(c, i) in audit.checks" :key="i">
              <t-card :bordered="false" class="check-card" :class="`check-${c.status}`">
                <div class="check-head">
                  <t-icon :name="statusIcon(c.status)" :class="`icon-${c.status}`" />
                  <span class="check-item">{{ c.item }}</span>
                  <t-tag :theme="statusTheme(c.status)" variant="light" size="small">{{ statusText(c.status) }}</t-tag>
                </div>
                <div class="check-detail">{{ c.detail }}</div>
                <div v-if="c.suggest" class="check-suggest">建议：{{ c.suggest }}</div>
              </t-card>
            </t-col>
          </t-row>

          <!-- 防火墙 & SSH -->
          <t-row :gutter="16" style="margin-bottom: 20px">
            <t-col :span="6">
              <t-card title="防火墙" :bordered="false">
                <div class="kv">
                  <span>状态</span>
                  <t-tag :theme="audit.firewall.active ? 'success' : 'warning'" variant="light">
                    {{ audit.firewall.active ? '运行中' : '未启用' }}
                  </t-tag>
                </div>
                <div class="kv">
                  <span>后端</span>
                  <span class="val">{{ audit.firewall.backend }}</span>
                </div>
              </t-card>
            </t-col>
            <t-col :span="6">
              <t-card title="SSH 配置" :bordered="false">
                <div class="kv"><span>端口</span><span class="val">{{ audit.ssh.port }}</span></div>
                <div class="kv">
                  <span>root 登录</span>
                  <t-tag :theme="audit.ssh.permit_root_login ? 'danger' : 'success'" variant="light" size="small">
                    {{ audit.ssh.permit_root_login ? '允许' : '禁止' }}
                  </t-tag>
                </div>
                <div class="kv">
                  <span>密码认证</span>
                  <t-tag :theme="audit.ssh.password_auth ? 'warning' : 'success'" variant="light" size="small">
                    {{ audit.ssh.password_auth ? '启用' : '禁用' }}
                  </t-tag>
                </div>
                <div class="kv">
                  <span>最大认证次数</span><span class="val">{{ audit.ssh.max_auth_tries }}</span>
                </div>
              </t-card>
            </t-col>
          </t-row>

          <!-- 监听端口 -->
          <h3 class="sec-title">监听端口（{{ audit.ports.length }}）</h3>
          <t-table :data="audit.ports" :columns="portColumns" row-key="port" size="small" hover :max-height="360">
            <template #proto="{ row }">
              <t-tag variant="outline" size="small">{{ row.proto }}</t-tag>
            </template>
            <template #addr="{ row }">
              <span class="mono">{{ row.address }}</span>
            </template>
          </t-table>
        </template>
      </t-loading>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { MessagePlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';

import { listAgents, securityAudit, type SecurityAudit } from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);
const audit = ref<SecurityAudit | null>(null);

const agentOptions = computed(() => agents.value);

const portColumns: PrimaryTableCol[] = [
  { colKey: 'port', title: '端口', width: 100 },
  { colKey: 'proto', title: '协议', width: 90 },
  { colKey: 'addr', title: '监听地址', width: 220 },
  { colKey: 'process', title: '进程', ellipsis: true },
];

function statusIcon(s: string) {
  return { pass: 'check-circle', warn: 'error-circle', fail: 'close-circle' }[s] || 'help-circle';
}
function statusTheme(s: string): 'success' | 'warning' | 'danger' | 'default' {
  const m: Record<string, 'success' | 'warning' | 'danger'> = { pass: 'success', warn: 'warning', fail: 'danger' };
  return m[s] ?? 'default';
}
function statusText(s: string) {
  return { pass: '通过', warn: '警告', fail: '风险' }[s] || s;
}

async function loadAgents() {
  loadingAgents.value = true;
  try {
    const list = await listAgents();
    // 默认跳过离线主机
    agents.value = list
      .filter((a) => a.online)
      .map((a) => ({
        label: `${a.hostname} (${a.ip})`,
        value: a.agent_id,
      }));
    if (!currentAgent.value && agents.value.length) currentAgent.value = agents.value[0].value;
  } finally {
    loadingAgents.value = false;
  }
}

async function fetchAudit() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    audit.value = await securityAudit(currentAgent.value);
  } catch (e: any) {
    MessagePlugin.error(e.message || '扫描失败');
  } finally {
    loading.value = false;
  }
}

onMounted(async () => {
  await loadAgents();
  fetchAudit();
});
</script>

<style scoped>
.sec-title {
  font-size: 15px;
  font-weight: 600;
  margin: 0 0 12px;
  color: var(--td-text-color-primary);
}
.check-card {
  border: 1px solid var(--td-component-stroke);
  border-radius: 8px;
  margin-bottom: 12px;
  height: 100%;
}
.check-pass { border-left: 3px solid #2ba471; }
.check-warn { border-left: 3px solid #ed7b2f; }
.check-fail { border-left: 3px solid #e34d59; }
.check-head {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-bottom: 6px;
}
.check-item {
  font-weight: 600;
  flex: 1;
  font-size: 13px;
}
.icon-pass { color: #2ba471; }
.icon-warn { color: #ed7b2f; }
.icon-fail { color: #e34d59; }
.check-detail {
  font-size: 12px;
  color: var(--td-text-color-secondary);
}
.check-suggest {
  font-size: 12px;
  color: #ed7b2f;
  margin-top: 6px;
  padding-top: 6px;
  border-top: 1px dashed var(--td-component-stroke);
}
.kv {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 8px 0;
  font-size: 13px;
  color: var(--td-text-color-secondary);
  border-bottom: 1px dashed var(--td-component-stroke);
}
.kv:last-child { border-bottom: none; }
.val { color: var(--td-text-color-primary); font-weight: 500; }
.mono { font-family: monospace; font-size: 12px; }
</style>
