-- GothicSaveSync Supabase setup
-- Execute this script in Supabase Dashboard > SQL Editor.
-- The FastAPI service uses the service-role key, so the bucket remains private.

create table if not exists public.save_packages (
    save_id text primary key
        check (save_id ~ '^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$'),
    filename text not null,
    storage_path text not null unique,
    size bigint not null check (size >= 0 and size <= 134217728),
    uploaded_at timestamptz not null default timezone('utc', now())
);

create index if not exists save_packages_uploaded_at_idx
    on public.save_packages (uploaded_at desc);

alter table public.save_packages enable row level security;

insert into storage.buckets (id, name, public, file_size_limit, allowed_mime_types)
values (
    'gothic-saves',
    'gothic-saves',
    false,
    134217728,
    array['application/octet-stream', 'application/x-gothicsavesync']
)
on conflict (id) do update
set public = excluded.public,
    file_size_limit = excluded.file_size_limit,
    allowed_mime_types = excluded.allowed_mime_types;

-- No public or anon policies are created intentionally.
-- Supabase service_role bypasses RLS and is used only by the FastAPI backend.
