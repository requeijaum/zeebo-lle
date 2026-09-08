# Zeebx → zeebo-lle: fatos GL aproveitáveis sem copiar código

Referência local examinada: `~/projects/zeebx-emu` (`edfda2f`).

## Regra de proveniência

O Zeebx é usado apenas para localizar comportamentos/ABI a confirmar. Seu código não é copiado: as implementações deste repositório são independentes, cobertas por testes próprios e baseadas no contrato público EGL/OpenGL ES. Fatos originados somente do Zeebx continuam condicionados à validação contra objeto/vtable viva do firmware.

## Fatos convergentes já adotados

- IGL legado possui 80 slots e não passa `this` aos métodos `gl*`.
- IEGL legado possui 28 slots; `eglSwapBuffers` é o slot 26.
- `gl*Pointer` altera o descritor do array, mas não habilita o array.
- Textura habilitada modula a cor corrente/do array.
- RGB565 é formato nativo importante; internamente o rasterizador pode normalizar para RGBA8.
- O filtro inicial de textura é linear; wrap inicial é repeat.
- Depth `LESS`, depth-write e blend `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` são partes necessárias do pipeline fixo.

Implementado por TDD no LLE:

- tabela completa dos 28 slots IEGL e apresentação no slot 26;
- upload RGBA8 e RGB565, bind por unidade e sampling bilinear/repeat;
- cor corrente `glColor4x`;
- estado de textura, depth e blend no frontend IGL;
- depth buffer e composição alpha no backend software;
- separação correta entre `gl*Pointer` e `glEnableClientState`.

## Próximos gaps de maior impacto

1. `glCompressedTexImage2D`: ATITC RGB/RGBA e formatos paletizados usados pelo corpus.
2. `glGetString`/`glGetIntegerv`: exige armazenamento de strings em memória guest sem inventar VA ou corromper o pool.
3. `eglGetProcAddress` + `IEGLSurfaceManip`: `SetSurfaceScale`, `SurfaceScaleEnable` e extensões retornadas por nome.
4. estados de fragmento restantes: alpha test, demais depth funcs/blend factors, culling, scissor e texture env.
5. recorte no near-plane e interpolação corrigida por perspectiva.
6. `TexParameterx`: nearest/linear e clamp-to-edge/repeat por textura.
7. apresentação contínua: ligar `eglSwapBuffers` ao sink/frame pacing do orquestrador.

## Gates

Cada item novo deve nascer de teste RED e terminar com prova por pixel/frame. Quando o firmware alcançar objetos IGL/IEGL vivos, validar a vtable estruturalmente e registrar slots observados antes de promovê-los de referência HLE para fato LLE.
