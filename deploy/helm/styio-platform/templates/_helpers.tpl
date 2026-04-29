{{- define "styio-platform.name" -}}
{{- default .Chart.Name .Values.nameOverride | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "styio-platform.fullname" -}}
{{- if .Values.fullnameOverride -}}
{{- .Values.fullnameOverride | trunc 63 | trimSuffix "-" -}}
{{- else -}}
{{- printf "%s-%s" .Release.Name (include "styio-platform.name" .) | trunc 63 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}

{{- define "styio-platform.labels" -}}
app.kubernetes.io/name: {{ include "styio-platform.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
app.kubernetes.io/version: {{ .Chart.AppVersion | quote }}
app.kubernetes.io/managed-by: {{ .Release.Service }}
{{- end -}}

{{- define "styio-platform.selectorLabels" -}}
app.kubernetes.io/name: {{ include "styio-platform.name" . }}
app.kubernetes.io/instance: {{ .Release.Name }}
{{- end -}}

{{- define "styio-platform.postgresDsn" -}}
{{- if .Values.postgres.externalDsn -}}
{{- .Values.postgres.externalDsn -}}
{{- else -}}
host={{ include "styio-platform.fullname" . }}-postgres port=5432 dbname={{ .Values.postgres.database }} user={{ .Values.postgres.username }} password=$(STYIO_PLATFORM_POSTGRES_PASSWORD)
{{- end -}}
{{- end -}}
