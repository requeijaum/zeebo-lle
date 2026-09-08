# Zeebx → zeebo-lle: fatos GL aproveitáveis sem copiar código

Referência local examinada: `~/projects/zeebx-emu` (`edfda2f`).

## Regra de proveniência

O Zeebx é usado apenas para localizar comportamentos/ABI a confirmar. Seu código não é copiado: as implementações deste repositório são independentes, cobertas por testes próprios e baseadas no contrato público EGL/OpenGL ES. Fatos originados somente do Zeebx continuam condicionados à validação contra objeto/vtable viva do firmware.

## Fatos convergentes já adotados

- IGL legado possui 80 slots e não passa `this` aos métodos `gl*`.
- IEGL legado possui 28 slots; `eglSwapBuffers` é o slot 26. A forma nova IEGL11 possui 31 slots e usa o slot 25 — são ABIs diferentes e não podem compartilhar uma constante.
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
3. Resolução real de extensões: observar os VAs retornados por `eglGetProcAddress` e só então registrá-los no bridge; `glDrawTexOES` não integra a vtable legada fixa.
4. `IEGLSurfaceManip`: reconhecer `SetSurfaceScale` no slot 4 apenas após `QueryInterface`/objeto vivo; não fabricar interface nem substituir a composição MDP.
5. estados de fragmento restantes: alpha test, demais depth funcs/blend factors, culling, scissor e texture env.
6. recorte no near-plane e interpolação corrigida por perspectiva.
7. `TexParameterx`: nearest/linear e clamp-to-edge/repeat por textura.
8. apresentação contínua: ligar `eglSwapBuffers` ao sink/frame pacing do orquestrador.

## Distinções ABI que não podem ser colapsadas

- O frontend atual implementa a ABI legada `IGL(80)/IEGL(28)`: sem `this` nos métodos GL/EGL; `eglGetProcAddress=8`; `eglSwapBuffers=26`.
- A ABI nova `IGLES11(148)/IEGL11(31)` usa `this` e retorno por ponteiro; não possui `GetProcAddress` na mesma posição e tem `SwapBuffers=25`.
- O bridge não deve inferir a ABI apenas pelo tamanho que passa numa heurística. A promoção para a forma nova exige ClassID/IID observado no guest e vtable viva validada contra segmentos executáveis do firmware.

## Gates

Cada item novo deve nascer de teste RED e terminar com prova por pixel/frame. Quando o firmware alcançar objetos IGL/IEGL vivos, validar a vtable estruturalmente e registrar slots observados antes de promovê-los de referência HLE para fato LLE.
