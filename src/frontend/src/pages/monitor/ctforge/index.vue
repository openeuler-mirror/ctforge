<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>CTForge eBPF 管理</span>
          <t-select
            v-model="currentAgent"
            :options="agentOptions"
            placeholder="选择主机"
            style="width: 260px"
            :loading="loadingAgents"
            @change="onAgentChange"
          />
        </t-space>
      </template>
      <template #actions>
        <t-space>
          <t-button theme="default" :loading="loadingStatus" @click="refreshStatus">刷新状态</t-button>
          <t-button theme="primary" :loading="loadingModules" @click="refreshModules">刷新模块</t-button>
        </t-space>
      </template>

      <t-alert theme="info" style="margin-bottom: 16px">
        <template #icon><t-icon name="info-circle" /></template>
        通过 CTForge 代理控制目标主机上的 ctforged 守护进程，管理 eBPF payload 模块的加载、卸载与状态查询。
      </t-alert>

      <!-- ctforged 状态 -->
      <t-card v-if="status" :bordered="false" title="守护进程状态" style="margin-bottom: 16px">
        <template #actions>
          <t-tag :theme="statusOk ? 'success' : 'danger'" variant="light">
            {{ statusOk ? '运行中' : '异常' }}
          </t-tag>
        </template>
        <pre class="json-pre">{{ JSON.stringify(status, null, 2) }}</pre>
      </t-card>

      <!-- 模块列表 -->
      <t-card :bordered="false" title="已加载模块" style="margin-bottom: 16px">
        <template #actions>
          <t-button size="small" theme="danger" :disabled="!selectedModule" :loading="acting" @click="onRmmod">
            卸载选中
          </t-button>
        </template>
        <t-table
          v-model:selected-row-keys="selectedRowKeys"
          :data="modules"
          :columns="moduleColumns"
          :loading="loadingModules"
          row-key="name"
          hover
          select-on-row-click
        >
          <template #empty>
            <t-empty description="暂无已加载模块" />
          </template>
        </t-table>
      </t-card>

      <!-- 加载模块 -->
      <t-card :bordered="false" title="加载模块" style="margin-bottom: 16px">
        <t-form :data="insmodForm" layout="inline" @submit="onInsmod">
          <t-form-item label="模块名">
            <t-input
              v-model="insmodForm.module"
              placeholder="输入 payload 模块名"
              style="width: 280px"
              @enter="onInsmod"
            />
          </t-form-item>
          <t-form-item>
            <t-button theme="primary" type="submit" :loading="acting">加载</t-button>
          </t-form-item>
        </t-form>
      </t-card>

      <!-- 子命令执行 -->
      <t-card :bordered="false" title="模块子命令">
        <t-form :data="modForm" layout="inline" @submit="onModCmd">
          <t-form-item label="模块名">
            <t-input v-model="modForm.module" placeholder="模块名" style="width: 180px" />
          </t-form-item>
          <t-form-item label="子命令">
            <t-input
              v-model="modForm.subcmd"
              placeholder="如: status / stats / config"
              style="width: 240px"
              @enter="onModCmd"
            />
          </t-form-item>
          <t-form-item>
            <t-button theme="primary" type="submit" :loading="acting">执行</t-button>
          </t-form-item>
        </t-form>

        <!-- 执行结果 -->
        <div v-if="cmdResult" style="margin-top: 16px">
          <t-divider />
          <pre class="json-pre">{{ cmdResult }}</pre>
        </div>
      </t-card>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue';
import { MessagePlugin } from 'tdesign-vue-next';

import {
  ctforgeLsmod,
  ctforgeStatus,
  ctforgeInsmod,
  ctforgeRmmod,
  ctforgeModCmd,
} from '@/api/ctforge';
import { http } from '@/api/agent';

// ---------- 状态 ----------
const currentAgent = ref('');
const agentOptions = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);

const status = ref<Record<string, unknown> | null>(null);
const statusOk = computed(() => status.value && !('error' in status.value));
const loadingStatus = ref(false);

const modules = ref<Record<string, unknown>[]>([]);
const loadingModules = ref(false);
const selectedRowKeys = ref<string[]>([]);
const selectedModule = computed(() => selectedRowKeys.value[0] || '');

const insmodForm = reactive({ module: '' });
const modForm = reactive({ module: '', subcmd: '' });
const acting = ref(false);
const cmdResult = ref('');

// ---------- 模块列表列定义 ----------
const moduleColumns = [
  { colKey: 'row-select', type: 'single', width: 50 },
  { title: '模块名', colKey: 'name', ellipsis: true },
  { title: '描述', colKey: 'desc', ellipsis: true },
  { title: '状态', colKey: 'state', width: 100 },
  { title: '引用计数', colKey: 'refcnt', width: 90 },
];

// ---------- 加载主机列表 ----------
async function loadAgents() {
  loadingAgents.value = true;
  try {
    const res = await http.get<never, { agents: { agent_id: string; name: string; online: boolean }[] }>('/agents');
    const list = res.agents || [];
    agentOptions.value = list
      .filter((a) => a.online)
      .map((a) => ({ label: a.name || a.agent_id, value: a.agent_id }));
    if (!currentAgent.value && agentOptions.value.length > 0) {
      currentAgent.value = agentOptions.value[0].value;
      onAgentChange();
    }
  } catch (e: unknown) {
    MessagePlugin.error(`加载主机列表失败: ${(e as Error).message}`);
  } finally {
    loadingAgents.value = false;
  }
}

// ---------- 数据加载 ----------
async function refreshStatus() {
  if (!currentAgent.value) return;
  loadingStatus.value = true;
  try {
    status.value = await ctforgeStatus(currentAgent.value);
  } catch (e: unknown) {
    status.value = { error: (e as Error).message };
  } finally {
    loadingStatus.value = false;
  }
}

async function refreshModules() {
  if (!currentAgent.value) return;
  loadingModules.value = true;
  try {
    const res = await ctforgeLsmod(currentAgent.value);
    modules.value = Array.isArray(res) ? res : [res];
    selectedRowKeys.value = [];
  } catch (e: unknown) {
    MessagePlugin.error(`加载模块列表失败: ${(e as Error).message}`);
    modules.value = [];
  } finally {
    loadingModules.value = false;
  }
}

function onAgentChange() {
  status.value = null;
  modules.value = [];
  cmdResult.value = '';
  refreshStatus();
  refreshModules();
}

// ---------- 操作 ----------
async function onInsmod() {
  if (!currentAgent.value || !insmodForm.module) return;
  acting.value = true;
  try {
    const res = await ctforgeInsmod(currentAgent.value, insmodForm.module);
    cmdResult.value = JSON.stringify(res, null, 2);
    MessagePlugin.success(`模块 ${insmodForm.module} 加载成功`);
    insmodForm.module = '';
    refreshModules();
  } catch (e: unknown) {
    MessagePlugin.error(`加载失败: ${(e as Error).message}`);
  } finally {
    acting.value = false;
  }
}

async function onRmmod() {
  if (!currentAgent.value || !selectedModule.value) return;
  acting.value = true;
  try {
    const res = await ctforgeRmmod(currentAgent.value, selectedModule.value);
    cmdResult.value = JSON.stringify(res, null, 2);
    MessagePlugin.success(`模块 ${selectedModule.value} 卸载成功`);
    refreshModules();
  } catch (e: unknown) {
    MessagePlugin.error(`卸载失败: ${(e as Error).message}`);
  } finally {
    acting.value = false;
  }
}

async function onModCmd() {
  if (!currentAgent.value || !modForm.module) return;
  acting.value = true;
  try {
    const res = await ctforgeModCmd(currentAgent.value, modForm.module, modForm.subcmd);
    cmdResult.value = JSON.stringify(res, null, 2);
  } catch (e: unknown) {
    MessagePlugin.error(`执行失败: ${(e as Error).message}`);
  } finally {
    acting.value = false;
  }
}

// ---------- 生命周期 ----------
onMounted(loadAgents);
</script>

<style scoped>
.json-pre {
  background: var(--td-bg-color-page);
  border-radius: 6px;
  padding: 12px;
  margin: 0;
  font-size: 12px;
  line-height: 1.6;
  max-height: 400px;
  overflow: auto;
  white-space: pre-wrap;
  word-break: break-all;
}
</style>
