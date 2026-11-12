# Paper ADC1 — Tema 4: False Sharing (contexto del proyecto)

Este archivo resume todo lo hablado hasta ahora para seguir trabajando desde la
laptop con Linux. **Todo es editable**: el abstract, el título, las referencias
y el enfoque del experimento todavía pueden cambiar.

## Curso y entregas

- **Curso:** EL-4314 Arquitectura de Computadores I, TEC, II semestre 2026.
- **Profesor:** Kervin Sánchez Herrera.
- **Grupo:** GrupoAW (en los proyectos 1–3: Walter Alfaro Ulate  y
  Allan Arrieta Quirós ). Confirmar quiénes van en el paper.
- **Formato obligatorio:** inglés, LaTeX, plantilla IEEE Transactions (`IEEEtran`).
  Se entregan las fuentes LaTeX y el PDF por Tec Digital.
- **Fechas:**
  1. **Abstract y referencias: 6 nov 2026, 6:00 pm** ← entrega actual
  2. Versión preliminar: 20 nov 2026, 6:00 pm
  3. Versión final: 4 dic 2026, 11:45 pm
- La guía completa está en `docs/Proyectos_Guia_temas_paper_ADC1_2026.pdf`.

## Tema elegido (tema 4 de la guía)

**Falso compartimiento y escalabilidad en procesadores multinúcleo.**

- **Pregunta de investigación:** ¿Cómo afecta la distribución de variables en
  líneas de caché al rendimiento de un programa paralelo?
- **Alcance sugerido:** comparar contadores independientes almacenados juntos
  y separados mediante alineamiento o relleno. Explicar el tráfico de
  coherencia y el costo en memoria.
- **Fuente inicial:** https://kernel.org/doc/html/latest/kernel-hacking/false-sharing.html
- **Comentarios del profe en clase:**
  - Esta entrega lleva solo abstract y referencias.
  - Usar la doc del kernel Linux y la fuente de Joe Mario (joemario.github.io).
  - Buscar más información sobre el autor.
  - La metodología del experimento se debe acordar con el profe.

## Conceptos clave (resumen)

- La caché se mantiene coherente **por línea** (64 B en x86-64), no por variable.
- **False sharing:** varios hilos en núcleos distintos escriben variables
  *independientes* que caen en la misma línea. El protocolo MESI invalida la
  línea en los otros núcleos en cada escritura, así que la línea "rebota" entre
  núcleos y el programa deja de escalar.
- Hace daño cuando (1) varios núcleos acceden a la misma línea y (2) al menos
  uno de esos accesos es una escritura.
- **Mitigaciones** (doc del kernel):
  - Poner la variable en su propia línea (padding o `alignas(64)` /
    `____cacheline_aligned`).
  - Reorganizar los campos de los structs.
  - Leer antes de escribir.
  - Usar contadores por CPU.
- **Costo de mitigar:** el padding gasta memoria, caché y TLB.
- **Detección:** `perf c2c` (líneas disputadas vía eventos HITM) y `pahole`
  (layout de structs por línea de caché).

## Autores de las fuentes

- **Doc del kernel:** la página dice "The kernel development community", pero
  el autor es **Feng Tang (Intel)**. Se verificó con el commit `911ac7979dde`
  ("Documentation: Add document for false sharing", 7 abr 2023), que aplicó
  Jonathan Corbet. Se cita como `tang2023kernel`.
- **Joe Mario:** ingeniero de rendimiento en Red Hat.
  - Escribió "C2C - False Sharing Detection in Linux Perf" (1 sep 2016).
  - Codesarrolló `perf c2c` con Don Zickus y Dick Fowles; está en upstream
    desde Linux 4.2.
  - Repo con ejemplos: https://github.com/joemario/perf-c2c-usage-files
    (incluye `false_sharing_example.c`).

## Archivos en esta carpeta

| Archivo | Qué es |
|---|---|
| `paper/paper.tex` | Paper IEEEtran: título, autores (Walter Alfaro, Allan Arrieta), abstract, keywords y `\nocite{*}`. Se trabaja en **Overleaf**; la copia local se sincroniza copiando y pegando |
| `paper/references.bib` | 10 referencias verificadas en BibTeX (estilo `IEEEtran`) |
| `paper/paper.pdf` | Compilación vieja (desactualizada) |
| `benchmarks/fs.c` | Benchmark: contadores juntos (8 B) vs separados (64 B), 1–6 hilos, `volatile ++` |
| `benchmarks/fs_atomic.c` | Igual, pero con incremento atómico (`__atomic_fetch_add`) |
| `docs/Proyectos_Guia_temas_paper_ADC1_2026.pdf` | Guía oficial de temas y fechas |
| `sim/mesi_sim.c` | Simulador MESI propio (N núcleos, bus snooping, layouts packed/padded/local/shared, prefetch adyacente). `make && ./sweep.sh` genera CSV en `results/sim/` |

**Compilar el paper en Linux** (requiere `texlive-publishers` o
`texlive-full`, que traen IEEEtran):

```bash
cd paper && pdflatex paper && bibtex paper && pdflatex paper && pdflatex paper
```

## Estado del abstract

- Ya está escrito en inglés en `paper.tex`.
- Dice que el experimento es en un **Intel Core i7-1260P (Alder Lake, híbrido
  P/E)** con cada hilo fijado a un núcleo, con microbenchmark de contadores
  juntos vs con padding vs acumulación por hilo, análisis con MESI, contadores
  de hardware y **`perf c2c` (HITM)**, y que cuantifica el costo en memoria
  del padding.
- No inventa resultados numéricos. Mantenerlo así hasta tener mediciones reales.
- Si cambia el procesador o la metodología, ajustar el abstract.

## Referencias (10, todas verificadas el 5 nov 2026)

| Clave | Fuente | Para qué sirve |
|---|---|---|
| `tang2023kernel` | Doc del kernel, F. Tang 2023 | Fuente del profe; patrones y mitigaciones |
| `mario2016c2c` | Blog de Joe Mario 2016 | Fuente del profe; cómo usar `perf c2c` y HITM |
| `mario_c2c_files` | Repo `perf-c2c-usage-files` | Programa de ejemplo y reportes |
| `perf_c2c_man` | Página de manual `perf-c2c(1)` | Definición oficial de la herramienta y de HITM |
| `intel2024opt` | Intel Optimization Manual vol. 1, rev. 051 (may 2024) | False sharing en Intel, prefetcher de línea adyacente, núcleos híbridos |
| `drepper2007memory` | Drepper 2007 | Jerarquía de memoria y costo de la coherencia en multihilo |
| `torrellas1994false` | IEEE TC 43(6):651–663, 1994, DOI 10.1109/12.286299 | Artículo clásico: false sharing vs localidad espacial |
| `liu2011sheriff` | OOPSLA 2011, pp. 3–18, DOI 10.1145/2048066.2048070 | Detección y mitigación automática |
| `hennessy2017ca` | Hennessy & Patterson, 6.ª ed., 2017 | Libro base: MESI y multiprocesadores |
| `nagarajan2020primer` | Primer, 2.ª ed., 2020 (acceso abierto) | Referencia formal de protocolos de coherencia |

- **Se eliminaron** por redundantes o porque no se pudieron verificar: el correo
  de Torvalds sobre `mmap_lock` (lore.kernel.org bloquea el acceso y el asunto
  no se pudo confirmar; ya lo cita la doc del kernel), Bolosky/Scott 1993,
  PREDATOR 2014 y Nanavati et al. 2013.
- En la versión final se pueden volver a agregar si el texto las necesita.

## Máquina del experimento (Ubuntu en la laptop; ya no se usa Windows)

- **CPU:** Intel Core i7-1260P (12.ª gen, Alder Lake, híbrido). Kernel
  7.0.0-34-generic, instalado directo (no es VM).
- **P-cores:** 4 núcleos con HT, CPUs lógicas 0–7 (pares 0/1, 2/3, 4/5, 6/7),
  4.7 GHz y L2 propia por núcleo.
- **E-cores:** 8 núcleos sin HT, CPUs 8–15, 3.4 GHz y L2 compartida en 2 grupos
  de 4.
- **L3:** 18 MiB, compartida por todos los núcleos.
- **Para medir en P-cores:** usar `taskset -c 0,2,4,6`, con un hilo por núcleo
  físico y hasta 4 hilos. No usar CPUs hermanas, porque comparten L1.
- **Idea extra (consultar al profe):** comparar P-cores, donde la línea viaja
  por la L3, con E-cores de un mismo grupo, que comparten L2.
- **Para el paper:** repetir cada medición varias veces, promediar y reportar
  la desviación.

## Plan acordado: trabajar en la laptop con Linux

1. **Ver el procesador:** correr `lscpu` y `uname -r` y anotar modelo, núcleos
   y kernel.
   - **Intel:** `perf c2c` funciona sin problema.
   - **AMD:** funciona con kernel ≥ 6.1 (usa IBS).
   - **Intel 12.ª gen+ (híbrido P/E):** fijar hilos a núcleos P con `taskset`.
   - Linux tiene que estar **instalado directo**, no en una VM.
2. **Instalar perf:**
   ```bash
   sudo apt install linux-tools-common linux-tools-generic linux-tools-$(uname -r)
   ```
   Si da error de permisos: `sudo sysctl kernel.perf_event_paranoid=-1`.
3. **Compilar y medir:**
   ```bash
   gcc -O2 -g -pthread benchmarks/fs_atomic.c -o fs
   ./fs
   sudo perf c2c record -- ./fs
   sudo perf c2c report --stdio
   ```
   - **Juntos:** se esperan muchos HITM en una sola línea, con cada hilo en un
     offset distinto (0x0, 0x8, 0x10…).
   - **Separados:** se espera casi ningún HITM.
   - Conviene separar el benchmark en dos ejecutables (o un argumento de línea
     de comandos) para grabar cada versión por aparte.
4. **Ajustar el abstract** al procesador real de la laptop.

## Ideas para la versión preliminar y la final

- **Experimento principal:**
  - Tiempo vs número de hilos para tres layouts: juntos, padding a 64 B y
    acumulación local por hilo.
  - Medir HITM con `perf c2c`.
  - Calcular el costo en memoria: 8 B vs 64 B por contador.
  - Opcional: probar padding de 128 B, porque el prefetcher de líneas
    adyacentes de Intel trae pares de líneas.
- **Conexión con los proyectos anteriores:**
  - **Proyecto 2 (simulador de caché en C; `../ADC1-P2-GrupoAW`):** es la mejor
    conexión. Se podría extender a 2 núcleos con caché privada y MESI, y contar
    las invalidaciones con trazas "juntos" vs "separados". Eso explica el
    tráfico de coherencia con un modelo propio.
  - **Proyecto 1 (ensamblador x86 16 bits):** solo para mostrar o explicar el
    ciclo `lock add` en ensamblador.
  - **Proyecto 3 (pipeline):** casi no se relaciona.
- Al escribir, distinguir resultados medidos, estimaciones y afirmaciones de
  terceros, como pide la guía.

## Pendientes inmediatos (antes del 6 nov, 6 pm)

- [x] Poner los nombres reales en `\author{...}` de `paper.tex`.
- [x] Verificar las referencias académicas marcadas arriba.
- [x] Ajustar el abstract al procesador y la metodología finales.
- [ ] Compilar y revisar el PDF.
- [ ] Subir `paper.tex`, `references.bib` y `paper.pdf` a Tec Digital.
