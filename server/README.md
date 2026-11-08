# GothicSaveSync Server

API HTTP para almacenar y recuperar paquetes `.gss` generados por el plugin.

## Ejecutar

```powershell
cd server
py -m venv .venv
.venv\Scripts\Activate.ps1
python -m pip install -r requirements.txt
uvicorn app:app --reload
```

## Desplegar en Render

1. Crear un servicio **Web Service** conectado al repositorio.
2. Establecer `Root Directory` como `server`.
3. Seleccionar `Docker` como entorno.
4. Render detectara este `Dockerfile` automaticamente.
5. No fijar el puerto manualmente: el contenedor usa la variable `PORT` de Render.
6. Añadir las variables de Supabase en el panel de Render.

El endpoint de comprobacion sera:

```text
https://<tu-servicio>.onrender.com/health
```

El almacenamiento local de Render puede ser efimero. Para conservar partidas tras reinicios se recomienda usar Supabase Storage.

### Configurar Supabase

1. Abre el SQL Editor de tu proyecto Supabase.
2. Ejecuta [supabase_schema.sql](supabase_schema.sql).
3. En Render, añade estas variables como secretos del servicio:

```text
SUPABASE_URL=https://<project-ref>.supabase.co
SUPABASE_SERVICE_ROLE_KEY=<service-role-key>
SUPABASE_STORAGE_BUCKET=gothic-saves
```

La `service_role` key solo debe existir en Render o en el archivo local
`server/.env`; nunca debe estar en el `.env` de Gothic ni en el repositorio. El script crea la tabla `save_packages` y el bucket
privado `gothic-saves`, con un límite de 128 MiB por paquete.

`SUPABASE_URL` debe ser la URL HTTP del proyecto
(`https://<project-ref>.supabase.co`), no la cadena de conexión PostgreSQL ni
la contraseña de la base de datos.

Si no se configuran las variables de Supabase, el servidor usa filesystem local
solo para desarrollo. Por defecto, los paquetes se guardan en `server/storage`.
Se puede cambiar la ruta:

```powershell
$env:GOTHICSAVE_STORAGE = "D:\GothicSaveSyncStorage"
```

## Autenticación

Todas las rutas requieren la cabecera `X-API-Key` con el valor de la variable
`GOTHICSAVE_API_TOKEN`. Excepción: `/health` es público.

```text
GOTHICSAVE_API_TOKEN=<token-largo-y-aleatorio>
```

Si la variable está vacía o no existe, la autenticación se desactiva y el
servidor muestra un aviso al arrancar. Úsalo así solo en desarrollo.

## Endpoints

| Método | Ruta | Uso |
| --- | --- | --- |
| `GET` | `/health` | Comprobar que el servicio está activo (público, sin API key) |
| `GET` | `/status` | Comprobar que el servicio y su almacenamiento están disponibles |
| `POST` | `/saves/{save_id}` | Subir o reemplazar un `.gss` como multipart `save` |
| `GET` | `/saves` | Listar partidas disponibles, ordenadas por `uploaded_at` descendente |
| `GET` | `/saves/{save_id}` | Obtener metadatos |
| `GET` | `/saves/{save_id}/download` | Descargar el paquete `.gss` |
| `DELETE` | `/saves/{save_id}` | Eliminar una partida |

Ejemplo de subida:

```powershell
curl.exe -X POST http://127.0.0.1:8000/saves/savegame1 `
	-H "X-API-Key: $env:GOTHICSAVE_API_TOKEN" `
	-F "save=@.\savegame1.gss"
```

Ejemplo de descarga:

```powershell
curl.exe -H "X-API-Key: $env:GOTHICSAVE_API_TOKEN" `
	-o savegame1.gss http://127.0.0.1:8000/saves/savegame1/download
```

### Control de conflictos

La subida puede incluir la cabecera opcional `X-Base-Epoch` con el valor
`uploaded_at_epoch` que el cliente tenía al descargar la partida. Si la partida
remota es más reciente que ese valor, el servidor responde `409 Conflict` con
`Remote save is newer.` y no sobrescribe nada. Sin la cabecera no se comprueba
nada y la subida sobrescribe la partida. Un valor que no es entero responde `400`.

```powershell
curl.exe -X POST http://127.0.0.1:8000/saves/savegame1 `
	-H "X-API-Key: $env:GOTHICSAVE_API_TOKEN" `
	-H "X-Base-Epoch: 1767225600" `
	-F "save=@.\savegame1.gss"
```

La API valida el identificador, la extensión y la cabecera `GSSPKG1` del paquete. El límite por defecto es de 64 MiB por archivo; se puede cambiar con `GOTHICSAVE_MAX_MB`. `uploaded_at` representa la última modificación remota del slot. Render proporciona TLS en el dominio del servicio.
