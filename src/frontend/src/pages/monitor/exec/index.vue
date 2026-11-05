<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>远程命令执行</span>
          <t-select v-model="currentAgent" :options="agentOptions" placeholder="选择主机" style="width: 260px" :loading="loadingAgents" />
        </t-space>
      </template>
      <template #actions>
        <t-button theme="primary" :loading="running" @click="onExec">执行</t-button>
      </template>

      <t-alert theme="warning" style="margin-bottom: 16px">
        <template #icon><t-icon name="error-circle" /></template>
        命令将在目标主机上以 agent 运行身份执行（通常为 root）。请谨慎操作，避免执行破坏性命令。所有操作均记录审计日志。
      </t-alert>

      <t-form :data="form" layout="inline" style="margin-bottom: 16px">
        <t-form-item label="命令" style="flex: 1">
          <t-input v-model="form.command" placeholder="如：df -h 或 ps aux | grep nginx" style="width: 100%; min-width: 400px" @enter="onExec" />
        </t-form-item>
        <t-form-item label="超时">
          <t-select v-model="form.timeout_sec" style="width: 100px">
            <t-option :value="30" label="30s" />
            <t-option :value="60" label="60s" />
            <t-option :value="120" label="120s" />
            <t-option :value="300" label="300s" />
          </t-select>
        </t-form-item>
        <t-form-item label="Shell">
          <t-select v-model="form.shell" style="width: 120px">
            <t-option value="/bin/bash" label="bash" />
            <t-option value="/bin/sh" label="sh" />
          </t-select>
        </t-form-item>
      </t-form>

      <!-- 快捷命令 -->
      <div style="margin-bottom: 16px">
        <span class="quick-label">快捷：</span>
        <t-tag v-for="c in quickCmds" :key="c.label" variant="outline" style="cursor: pointer; margin-right: 8px" @click="quickRun(c)">
          {{ c.label }}
        </t-tag>
      </div>

      <t-loading :loading="running">
        <template v-if="result">
          <t-card :bordered="false" class="result-card">
            <template #title>
              <t-space>
                <t-tag :theme="result.ok ? 'success' : 'danger'" variant="light">
                  {{ result.ok ? '成功' : '失败' }}
                </t-tag>
                <span>退出码: {{ result.exit_code }}</span>
                <span>耗时: {{ result.duration_ms }}ms</span>
              </t-space>
            </template>
            <template #actions>
              <t-button theme="default" size="small" @click="result = null">清空</t-button>
            </template>
            <div v-if="result.stdout" class="output-block">
              <div class="output-label">标准输出</div>
              <pre class="output">{{ result.stdout }}</pre>
            </div>
            <div v-if="result.stderr" class="output-block">
              <div class="output-label error">标准错误</div>
              <pre class="output error">{{ result.stderr }}</pre>
            </div>
            <div v-if="!result.stdout && !result.stderr" class="output-empty">（无输出）</div>
          </t-card>
        </template>
      </t-loading>

      <!-- 执行历史 -->
      <h3 class="sec-title" style="margin-top: 24px">最近执行</h3>
      <t-table :data="history" :columns="histColumns" row-key="time" hover size="small" :max-height="300">
        <template #time="{ row }">{{ row.time }}</template>
        <template #cmd="{ row }"><span class="mono">{{ row.command }}</span></template>
        <template #ok="{ row }">
          <t-tag :theme="row.ok ? 'success' : 'danger'" variant="light" size="small">{{ row.ok ? '成功' : '失败' }}</t-tag>
        </template>
        <template #code="{ row }">{{ row.exit_code }}</template>
        <template #dur="{ row }">{{ row.duration_ms }}ms</template>
      </t-table>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { MessagePlugin } from 'tdesign-vue-next';
import dayjs from 'dayjs';

import { listAgents, execCommand, type CmdResult } from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const running = ref(false);
const form = ref({ command: '', timeout_sec: 60, shell: '/bin/bash' });
const result = ref<CmdResult | null>(null);
const history = ref<any[]>([]);

const agentOptions = computed(() => agents.value);

const quickCmds = [
  { label: '磁盘使用', cmd: 'df -h' },
  { label: '内存使用', cmd: 'free -h' },
  { label: '进程 TOP10', cmd: 'ps aux --sort=-%cpu | head -11' },
  { label: '监听端口', cmd: 'ss -tulnp' },
  { label: '最近登录', cmd: 'last -n 10' },
  { label: '系统信息', cmd: 'uname -a' },
  { label: '网络连接', cmd: 'ss -tan | head -30' },
];

const histColumns = [
  { colKey: 'time', title: '时间', width: 160 },
  { colKey: 'cmd', title: '命令', ellipsis: true },
  { colKey: 'ok', title: '结果', width: 80 },
  { colKey: 'code', title: '退出码', width: 80 },
  { colKey: 'dur', title: '耗时', width: 100 },
];

async function loadAgents() {
  loadingAgents.value = true;
  try {
    const list = await listAgents();
    agents.value = list.map((a) => ({
      label: `${a.hostname} (${a.ip})${a.online ? '' : ' [离线]'}`,
      value: a.agent_id,
    }));
    if (!currentAgent.value && agents.value.length) currentAgent.value = agents.value[0].value;
  } finally {
    loadingAgents.value = false;
  }
}

async function onExec() {
  if (!form.value.command.trim()) {
    MessagePlugin.warning('请输入命令');
    return;
  }
  if (!currentAgent.value) {
    MessagePlugin.warning('请选择主机');
    return;
  }
  running.value = true;
  try {
    const res = await execCommand(currentAgent.value, form.value.command, form.value.timeout_sec);
    result.value = res;
    history.value.unshift({
      time: dayjs().format('HH:mm:ss'),
      command: form.value.command,
      ...res,
    });
    if (history.value.length > 20) history.value.pop();
  } catch (e: any) {
    MessagePlugin.error(e.message || '执行失败');
  } finally {
    running.value = false;
  }
}

function quickRun(c: { label: string; cmd: string }) {
  form.value.command = c.cmd;
  onExec();
}

onMounted(loadAgents);
</script>

<style scoped>
.quick-label {
  font-size: 12px;
  color: var(--td-text-color-secondary);
}
.result-card {
  background: var(--td-bg-color-container-hover);
}
.output-block {
  margin-top: 12px;
}
.output-label {
  font-size: 12px;
  color: var(--td-text-color-secondary);
  margin-bottom: 4px;
}
.output-label.error {
  color: #e34d59;
}
.output {
  background: #1e1e1e;
  color: #d4d4d4;
  padding: 12px;
  border-radius: 6px;
  font-size: 12px;
  line-height: 1.6;
  max-height: 400px;
  overflow: auto;
  white-space: pre-wrap;
  word-break: break-all;
}
.output.error {
  background: #2d1e1e;
  color: #f0a8a8;
}
.output-empty {
  padding: 24px;
  text-align: center;
  color: var(--td-text-color-placeholder);
}
.mono { font-family: monospace; font-size: 12px; }
.sec-title {
  font-size: 15px;
  font-weight: 600;
  margin: 0 0 12px;
  color: var(--td-text-color-primary);
}
</style>
