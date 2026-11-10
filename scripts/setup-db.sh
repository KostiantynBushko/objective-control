#!/usr/bin/env bash
#
# Creates the PostgreSQL role and database that hold the enrolled faces.
#
#   scripts/setup-db.sh
#   scripts/setup-db.sh --db-name=faces --db-user=faces --db-password=secret
#
# The script needs a PostgreSQL superuser. It uses "sudo -u postgres psql"
# when that is available, and --admin-dsn=<connection string> otherwise.
#
# The schema itself is created by the application when it connects, so this
# script only has to create the role and the database.

set -euo pipefail

DB_NAME="${OBJECTIVE_CONTROL_DB_NAME:-objective_control}"
DB_USER="${OBJECTIVE_CONTROL_DB_USER:-objective_control}"
DB_PASSWORD="${OBJECTIVE_CONTROL_DB_PASSWORD:-}"
DB_HOST="${OBJECTIVE_CONTROL_DB_HOST:-127.0.0.1}"
DB_PORT="${OBJECTIVE_CONTROL_DB_PORT:-5432}"
ADMIN_DSN=""
APPLY_SCHEMA=1

usage() {
    cat <<'EOF'
Usage: scripts/setup-db.sh [options]

Options:
  --db-name=<name>      Database to create (default: objective_control)
  --db-user=<name>      Role to create (default: objective_control)
  --db-password=<pw>    Password for the role. A random one is generated
                        when this is not given.
  --db-host=<host>      Host the database runs on (default: 127.0.0.1)
  --db-port=<port>      Port the database listens on (default: 5432)
  --admin-dsn=<dsn>     Connect as this superuser instead of using
                        "sudo -u postgres psql".
  --no-schema           Do not apply sql/schema.sql at the end.
  -h, --help            Show this text and exit.
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --db-name=*)     DB_NAME="${1#*=}" ;;
        --db-user=*)     DB_USER="${1#*=}" ;;
        --db-password=*) DB_PASSWORD="${1#*=}" ;;
        --db-host=*)     DB_HOST="${1#*=}" ;;
        --db-port=*)     DB_PORT="${1#*=}" ;;
        --admin-dsn=*)   ADMIN_DSN="${1#*=}" ;;
        --no-schema)     APPLY_SCHEMA=0 ;;
        -h|--help)       usage; exit 0 ;;
        *) echo "Unknown option '$1'." >&2; usage >&2; exit 2 ;;
    esac
    shift
done

if [ -z "$DB_PASSWORD" ]; then
    if command -v openssl >/dev/null 2>&1; then
        DB_PASSWORD="$(openssl rand -base64 18 | tr -d '/+=' | cut -c1-20)"
    else
        DB_PASSWORD="$(head -c 24 /dev/urandom | base64 | tr -d '/+=' | cut -c1-20)"
    fi
    echo "Generated a password for role '$DB_USER'."
fi

# Builds a connection string that points at $DB_NAME, based on $ADMIN_DSN.
#
# psql ignores a connection string that is given as a plain argument when
# --dbname is passed as well, so the database name is baked into the
# connection string instead of being passed separately.
admin_dsn_for_db() {
    local dsn="$1" db="$2"
    case "$dsn" in
        postgres://*|postgresql://*)
            local scheme="${dsn%%://*}"
            local rest="${dsn#*://}"
            local authority="${rest%%/*}"
            local tail="${rest#"$authority"}"
            local params=""
            case "$tail" in
                *\?*) params="?${tail#*\?}" ;;
            esac
            printf '%s://%s/%s%s' "$scheme" "$authority" "$db" "$params"
            ;;
        *)
            # The last dbname in a connection string wins, so appending one
            # overrides the database of the original string.
            printf '%s dbname=%s' "$dsn" "$db"
            ;;
    esac
}

# Runs a superuser statement. Either through a local "sudo -u postgres"
# (the usual way on a Debian/Ubuntu machine) or through an admin DSN.
# The extra arguments are passed on to psql.
as_superuser() {
    if [ -n "$ADMIN_DSN" ]; then
        psql "$ADMIN_DSN" -v ON_ERROR_STOP=1 "$@"
    else
        if ! command -v sudo >/dev/null 2>&1; then
            echo "Neither sudo nor --admin-dsn is available." >&2
            echo "Run this script as a PostgreSQL superuser, or pass" >&2
            echo "--admin-dsn=<connection string>." >&2
            exit 1
        fi
        sudo -u postgres psql -v ON_ERROR_STOP=1 "$@"
    fi
}

# The same, but connected to the database that is being set up.
as_superuser_in_db() {
    if [ -n "$ADMIN_DSN" ]; then
        psql "$(admin_dsn_for_db "$ADMIN_DSN" "$DB_NAME")" \
            -v ON_ERROR_STOP=1 "$@"
    else
        if ! command -v sudo >/dev/null 2>&1; then
            echo "Neither sudo nor --admin-dsn is available." >&2
            echo "Run this script as a PostgreSQL superuser, or pass" >&2
            echo "--admin-dsn=<connection string>." >&2
            exit 1
        fi
        sudo -u postgres psql -v ON_ERROR_STOP=1 --dbname="$DB_NAME" "$@"
    fi
}

# The role and the database are only created when they are missing, so the
# script can be run again on an existing installation.
as_superuser --set=db_user="$DB_USER" --set=db_password="$DB_PASSWORD" <<'SQL'
SELECT format('CREATE ROLE %I LOGIN PASSWORD %L', :'db_user', :'db_password')
WHERE NOT EXISTS (SELECT FROM pg_catalog.pg_roles WHERE rolname = :'db_user')
\gexec
SQL

as_superuser --set=db_name="$DB_NAME" --set=db_user="$DB_USER" <<'SQL'
SELECT format('CREATE DATABASE %I OWNER %I', :'db_name', :'db_user')
WHERE NOT EXISTS (SELECT FROM pg_catalog.pg_database WHERE datname = :'db_name')
\gexec
SQL

echo "Role '$DB_USER' and database '$DB_NAME' are ready."

# The role needs the right to create the table. PostgreSQL 15 and later no
# longer grant CREATE on the public schema to everybody, so it is granted
# explicitly.
as_superuser_in_db --set=db_user="$DB_USER" <<'SQL'
SELECT format('GRANT USAGE, CREATE ON SCHEMA public TO %I', :'db_user')
\gexec
SQL

if [ "$APPLY_SCHEMA" -eq 1 ]; then
    schema="$(dirname "$0")/../third_party/face-detection/sql/schema.sql"
    if [ -f "$schema" ]; then
        echo "Applying the face-enrolment schema ..."
        PGPASSWORD="$DB_PASSWORD" psql \
            "host=$DB_HOST port=$DB_PORT dbname=$DB_NAME user=$DB_USER" \
            -v ON_ERROR_STOP=1 -f "$schema"
    else
        echo "Schema file not found at '$schema'; skipping." >&2
    fi
fi

echo
echo "Use this connection string:"
echo "  postgres://$DB_USER:$DB_PASSWORD@$DB_HOST:$DB_PORT/$DB_NAME"
echo
echo "Export it as DATABASE_URL to avoid passing --face-db every time:"
echo "  export DATABASE_URL='postgres://$DB_USER:$DB_PASSWORD@$DB_HOST:$DB_PORT/$DB_NAME'"
