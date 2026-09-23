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

El endpoint de comprobacion sera:

```text
https://<tu-servicio>.onrender.com/health
```

El almacenamiento local de Render puede ser efimero. Para conservar partidas tras reinicios hay que añadir un Persistent Disk o sustituir `server/storage` por almacenamiento de objetos.

Por defecto, los paquetes se guardan en `server/storage`. Se puede cambiar la ruta:

```powershell
$env:GOTHICSAVE_STORAGE = "D:\GothicSaveSyncStorage"
```

## Endpoints

| Método | Ruta | Uso |
| --- | --- | --- |
| `GET` | `/health` | Comprobar que el servicio está activo |
| `GET` | `/status` | Comprobar que el servicio y su almacenamiento están disponibles |
| `POST` | `/saves/{save_id}` | Subir o reemplazar un `.gss` como multipart `save` |
| `GET` | `/saves` | Listar partidas disponibles, ordenadas por `uploaded_at` descendente |
| `GET` | `/saves/{save_id}` | Obtener metadatos |
| `GET` | `/saves/{save_id}/download` | Descargar el paquete `.gss` |
| `DELETE` | `/saves/{save_id}` | Eliminar una partida |

Ejemplo de subida:

```powershell
curl.exe -X POST http://127.0.0.1:8000/saves/savegame1 `
	-F "save=@.\savegame1.gss"
```

Ejemplo de descarga:

```powershell
curl.exe -o savegame1.gss http://127.0.0.1:8000/saves/savegame1/download
```

La API valida el identificador, la extensión y la cabecera `GSSPKG1` del paquete. El límite inicial es de 128 MiB por archivo. `uploaded_at` representa la última modificación remota del slot. Para producción todavía faltan autenticación y almacenamiento de objetos; Render proporciona TLS en el dominio del servicio.
