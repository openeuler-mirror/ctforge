<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>硬件信息</span>
          <t-select v-model="currentAgent" :options="agentOptions" placeholder="选择主机" style="width: 260px" :loading="loadingAgents" @change="fetchHw" />
        </t-space>
      </template>
      <template #actions>
        <t-button theme="primary" :loading="loading" @click="fetchHw">刷新</t-button>
      </template>

      <t-loading :loading="loading">
        <template v-if="hw">
          <!-- CPU / 内存 / 主板 -->
          <t-row :gutter="12">
            <t-col :span="4">
              <t-card :bordered="false" class="hw-card"><template #title><t-icon name="cpu" style="margin-right:4px" /> CPU</template>
                <div class="hw-model">{{ hw.cpu_model || '-' }}</div>
                <div class="hw-kv"><span>物理核</span><b>{{ hw.cpu_cores }}</b></div>
                <div class="hw-kv"><span>线程</span><b>{{ hw.cpu_threads }}</b></div>
                <div class="hw-kv" v-if="hw.cpu_mhz"><span>频率</span><b>{{ hw.cpu_mhz.toFixed(0) }} MHz</b></div>
              </t-card>
            </t-col>
            <t-col :span="4">
              <t-card :bordered="false" class="hw-card"><template #title><t-icon name="laptop" style="margin-right:4px" /> 内存</template>
                <div class="hw-model">{{ formatKB(hw.mem_total_kb) }}</div>
                <div class="hw-kv"><span>Swap</span><b>{{ formatKB(hw.swap_total_kb) }}</b></div>
              </t-card>
            </t-col>
            <t-col :span="4">
              <t-card :bordered="false" class="hw-card"><template #title><t-icon name="desktop" style="margin-right:4px" /> 平台</template>
                <div class="hw-kv"><span>虚拟化</span><b>{{ hw.virtualization || '-' }}</b></div>
                <div class="hw-kv" v-if="hw.bios_vendor"><span>BIOS</span><b>{{ hw.bios_vendor }}</b></div>
                <div class="hw-kv" v-if="hw.board_name"><span>主板</span><b>{{ hw.board_name }}</b></div>
                <div class="hw-kv" v-if="hw.machine_id"><span>Machine ID</span><b class="mono">{{ hw.machine_id.slice(0, 16) }}…</b></div>
              </t-card>
            </t-col>
          </t-row>

          <!-- 磁盘 -->
          <t-card :bordered="false" style="margin-top: 12px">
            <template #title><t-icon name="folder-open" style="margin-right:4px" /> 磁盘（{{ hw.disks?.length || 0 }}）</template>
            <t-table :data="hw.disks" :columns="diskColumns" row-key="name" size="small">
              <template #type="{ row }">
                <t-tag :theme="row.type === 'ssd' ? 'primary' : 'warning'" variant="outline" size="small">
                  {{ row.type.toUpperCase() }}
                </t-tag>
              </template>
              <template #size="{ row }">{{ row.size_gb.toFixed(0) }} GB</template>
              <template #mount="{ row }">{{ row.mount_on || '-' }}</template>
            </t-table>
          </t-card>

          <!-- 网卡 -->
          <t-card :bordered="false" style="margin-top: 12px">
            <template #title><t-icon name="internet" style="margin-right:4px" /> 网卡（{{ hw.net_cards?.length || 0 }}）</template>
            <t-table :data="hw.net_cards" :columns="nicColumns" row-key="name" size="small">
              <template #mac="{ row }"><span class="mono">{{ row.mac }}</span></template>
            </t-table>
          </t-card>
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

import { getHardware, listAgents, type Hardware } from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);
const hw = ref<Hardware | null>(null);

const agentOptions = computed(() => agents.value);

const diskColumns: PrimaryTableCol[] = [
  { colKey: 'name', title: '设备', width: 100 },
  { colKey: 'model', title: '型号', ellipsis: true },
  { colKey: 'size', title: '容量', width: 100 },
  { colKey: 'type', title: '类型', width: 80 },
  { colKey: 'mount', title: '挂载点', width: 140 },
  { colKey: 'fs_type', title: '文件系统', width: 100 },
];
const nicColumns: PrimaryTableCol[] = [
  { colKey: 'name', title: '接口', width: 120 },
  { colKey: 'mac', title: 'MAC', width: 180 },
  { colKey: 'speed', title: '速率', width: 120 },
  { colKey: 'driver', title: '驱动', width: 160 },
];

function formatKB(kb: number) {
  if (kb >= 1024 * 1024) return (kb / 1024 / 1024).toFixed(1) + ' GB';
  if (kb >= 1024) return (kb / 1024).toFixed(0) + ' MB';
  return kb + ' KB';
}

async function loadAgents() {
  loadingAgents.value = true;
  try {
    const list = await listAgents();
    agents.value = list
      .filter((a) => a.approval === 'approved')
      .map((a) => ({ label: `${a.hostname} (${a.ip})${a.online ? '' : ' [离线]'}`, value: a.agent_id }));
    if (!currentAgent.value && agents.value.length) currentAgent.value = agents.value[0].value;
  } finally {
    loadingAgents.value = false;
  }
}

async function fetchHw() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    hw.value = await getHardware(currentAgent.value);
  } catch (e: any) {
    MessagePlugin.error(e.message || '获取失败');
  } finally {
    loading.value = false;
  }
}

onMounted(async () => {
  await loadAgents();
  fetchHw();
});
</script>

<style scoped>
.hw-card { min-height: 140px; }
.hw-model { font-size: 13px; font-weight: 600; margin-bottom: 12px; line-height: 1.4; }
.hw-kv { display: flex; justify-content: space-between; padding: 4px 0; font-size: 12px; }
.hw-kv span { color: var(--td-text-color-secondary); }
.hw-kv b { color: var(--td-text-color-primary); }
.mono { font-family: monospace; font-size: 12px; }
</style>
