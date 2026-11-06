<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>网络拓扑</span>
          <t-tag variant="outline" size="small">{{ serverInfo.label || serverInfo.id }}</t-tag>
        </t-space>
      </template>
      <template #actions>
        <t-button theme="primary" :loading="loading" @click="fetchTopo">刷新</t-button>
      </template>

      <div v-if="!nodes.length" class="empty-hint">
        暂无已批准的主机。先到【批准管理】中批准客户端。
      </div>
      <div ref="chartEl" class="topo-chart"></div>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { onMounted, onUnmounted, ref, shallowRef } from 'vue';
import { useRouter } from 'vue-router';
import * as echarts from 'echarts';

import { getTopology } from '@/api/agent';

const router = useRouter();
const chartEl = ref<HTMLElement>();
const chart = shallowRef<echarts.ECharts | null>(null);
const loading = ref(false);
const nodes = ref<any[]>([]);
const serverInfo = ref<any>({});
let timer: number | undefined;
/* 每台主机的延迟趋势缓存（最近 30 个采样点） */
const latTrend = new Map<string, number[]>();

function latencyTrend(agentId: string): number[] {
  return latTrend.get(agentId) || [];
}

function recordLatency(id: string, ms: number) {
  if (ms == null || ms <= 0) return;
  const arr = latTrend.get(id) || [];
  arr.push(ms);
  if (arr.length > 30) arr.shift();
  latTrend.set(id, arr);
}

/* 用 ASCII 火花线画趋势 */
function sparkline(values: number[]): string {
  if (values.length === 0) return '';
  const blocks = '▁▂▃▄▅▆▇█';
  const max = Math.max(...values);
  const min = Math.min(...values);
  const range = max - min || 1;
  return values
    .map((v) => {
      const idx = Math.round(((v - min) / range) * (blocks.length - 1));
      return `<span style="color:#4fc3f7">${blocks[idx]}</span>`;
    })
    .join('');
}

async function fetchTopo() {
  loading.value = true;
  try {
    const d = await getTopology();
    serverInfo.value = d.server || {};
    nodes.value = d.nodes || [];
    (d.nodes || []).forEach((n: any) => recordLatency(n.id, n.latency || 0));
    render(d);
  } finally {
    loading.value = false;
  }
}

function render(d: any) {
  if (!chartEl.value) return;
  if (!chart.value) {
    chart.value = echarts.init(chartEl.value);
    chart.value.on('click', (params: any) => {
      if (params.dataType === 'node' && params.data?.type === 'agent') {
        router.push({ path: '/monitor/detail', query: { agent: params.data.id, hostname: params.data.label } });
      }
    });
  }
  const serverId = d.server?.id || 'server';
  const graphNodes: any[] = [];
  const graphLinks: any[] = [];

  // 中心：服务端
  graphNodes.push({
    id: serverId,
    name: serverInfo.value.label || serverId,
    symbolSize: 90,
    category: 0,
    type: 'server',
    itemStyle: { color: '#0594fa' },
    label: { fontSize: 13, fontWeight: 'bold' },
    value: '',
  });

  // 客户端节点：延迟决定颜色
  const agentCount = (d.nodes || []).length;
  (d.nodes || []).forEach((a: any, i: number) => {
    const angle = (2 * Math.PI * i) / Math.max(agentCount, 1);
    const r = agentCount > 8 ? 280 : 200;
    const lat = a.latency || 0;
    const color = !a.online ? '#9e9e9e' : lat < 50 ? '#2ba471' : lat < 200 ? '#ed7b2f' : '#e34d59';
    const alerts = a.alerts || 0;
    graphNodes.push({
      id: a.id,
      name: a.label,
      type: 'agent',
      x: undefined, y: undefined,
      symbolSize: a.online ? 60 : 50,
      itemStyle: {
        color,
        borderColor: alerts > 0 ? '#e34d59' : 'transparent',
        borderWidth: alerts > 0 ? 3 : 0,
      },
      label: { fontSize: 11 },
      value: `${a.ip}\n${a.os || ''}`,
      // 用 force 布局时由力导向自动定位，这里给初始位置避免重叠
      fixed: false,
      initialX: Math.cos(angle) * r,
      initialY: Math.sin(angle) * r,
    });
    graphLinks.push({
      source: serverId,
      target: a.id,
      lineStyle: {
        color: a.online ? color : '#bdbdbd',
        width: a.online ? Math.min(3, 1 + lat / 100) : 1,
        type: a.online ? 'solid' : 'dashed',
        curveness: 0.08,
      },
      label: { show: a.online && lat > 0, formatter: `${lat.toFixed(0)}ms`, fontSize: 10 },
    });
  });

  chart.value.setOption({
    tooltip: {
      formatter: (p: any) => {
        if (p.dataType !== 'node') return '';
        if (p.data.type === 'server') return `<b>${p.data.name}</b><br/>服务端`;
        const a = nodes.value.find((n: any) => n.id === p.data.id);
        if (!a) return p.data.name;
        const latHist = latencyTrend(a.id);
        const latHtml = latHist.length > 1
          ? `<br/>延迟趋势（最近${latHist.length}次）：<br/>${sparkline(latHist)}ms`
          : '';
        return `<b>${a.label}</b><br/>
          IP：${a.ip}<br/>
          系统：${a.os || '-'}<br/>
          内核：${a.kernel || '-'}<br/>
          延迟：${(a.latency || 0).toFixed(0)}ms${latHtml}<br/>
          CPU：${(a.cpu || 0).toFixed(1)}% 内存：${(a.mem || 0).toFixed(1)}%<br/>
          状态：${a.online ? '在线' : '离线'}${a.alerts > 0 ? ` · 告警 ${a.alerts} 条` : ''}`;
      },
    },
    series: [{
      type: 'graph',
      layout: 'force',
      force: {
        repulsion: 400,
        edgeLength: [120, 260],
        gravity: 0.08,
      },
      roam: true,
      draggable: true,
      label: { show: true, position: 'bottom' },
      data: graphNodes,
      links: graphLinks,
      categories: [{ name: 'server' }, { name: 'agent' }],
    }],
  });
}

function onResize() { chart.value?.resize(); }

onMounted(() => {
  fetchTopo();
  timer = window.setInterval(fetchTopo, 10000);
  window.addEventListener('resize', onResize);
});
onUnmounted(() => {
  window.clearInterval(timer);
  window.removeEventListener('resize', onResize);
  chart.value?.dispose();
});
</script>

<style scoped>
.topo-chart {
  height: calc(100vh - 240px);
  min-height: 520px;
}
.empty-hint {
  padding: 60px;
  text-align: center;
  color: var(--td-text-color-placeholder);
}
</style>
