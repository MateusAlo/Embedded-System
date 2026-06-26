import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

# Configurações gerais da figura
fig, ax = plt.subplots(figsize=(15, 6))

# Definição das tarefas (Eixo Y) - Ordem decrescente de prioridade
tasks = ['Serial (P1)', 'Ambiente (P2)', 'Display (P3)', 'IMU (P5)', 'Queda (P6)']
y_positions = [10, 20, 30, 40, 50]
ax.set_yticks(y_positions)
ax.set_yticklabels(tasks)

# Dados de execução rigorosa baseada em prioridade: (início, duração) em milissegundos
# O tempo total de CPU respeita a preempção do FreeRTOS
exec_data = {
    'IMU (P5)': [(0, 1), (10, 1), (20, 1), (30, 1), (40, 1), (50, 1), (60, 1)],
    'Display (P3)': [(1, 9), (11, 1), (51, 9), (61, 1)], # Requer 10ms totais por ativação
    'Ambiente (P2)': [(12, 2), (16, 4), (21, 9), (31, 9), (41, 6)], # Requer 15ms totais por ativação
    'Queda (P6)': [(14, 2)], # Evento assíncrono de hardware via semáforo
    'Serial (P1)': [(47, 3), (62, 2)] # Executa apenas quando a CPU está ociosa
}

# Esquema de cores
colors = {
    'IMU (P5)': '#FF8C00',
    'Display (P3)': '#1E90FF',
    'Ambiente (P2)': '#32CD32',
    'Queda (P6)': '#FF0000',
    'Serial (P1)': '#8A2BE2'
}

# Renderização dos blocos de execução na linha do tempo
for task, data in exec_data.items():
    y_pos = y_positions[tasks.index(task)]
    ax.broken_barh(data, (y_pos - 4, 8), facecolors=colors[task], edgecolor='black')

# Sinalização dos eventos de preempção e interrupção do sistema
for t in [10, 20, 30, 40, 50, 60]:
    ax.axvline(x=t, color='gray', linestyle='--', linewidth=1)
    ax.text(t + 0.2, 45, 'Wakeup IMU', fontsize=9, color='gray')

ax.axvline(x=14, color='red', linestyle='--', linewidth=1)
ax.text(14.2, 55, 'Interrupção Queda (LED ligado!)', fontsize=9, color='red')

# Textos indicativos das trocas de contexto (Context Switch)
ax.text(10, 30, 'Preempção (IMU) →', fontsize=9, verticalalignment='center', horizontalalignment='right')
ax.text(14, 20, 'Preempção (ISR) →', fontsize=9, verticalalignment='center', horizontalalignment='right')
ax.text(20, 20, 'Preempção (IMU) →', fontsize=9, verticalalignment='center', horizontalalignment='right')
ax.text(40, 20, 'Preempção (IMU) →', fontsize=9, verticalalignment='center', horizontalalignment='right')
ax.text(50, 10, 'Preempção (IMU) →', fontsize=9, verticalalignment='center', horizontalalignment='right')
ax.text(60, 30, 'Preempção (IMU) →', fontsize=9, verticalalignment='center', horizontalalignment='right')

# Limites e formatação visual
ax.set_xlabel('Tempo (ms)', fontsize=12)
ax.set_title('Diagrama de Escalonamento Preemptivo FreeRTOS (0-64ms)', fontsize=14)
ax.set_xlim(0, 64)
ax.set_xticks(range(0, 65, 4))
ax.grid(True, axis='x', linestyle=':', alpha=0.7)

# Geração de legendas
patches = [mpatches.Patch(color=color, label=task) for task, color in colors.items()]
ax.legend(handles=patches, loc='upper right', bbox_to_anchor=(1, 0.95))

plt.tight_layout()
plt.savefig('gantt_freertos.png', dpi=300)