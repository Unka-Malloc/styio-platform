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

{{- define "styio-platform.objectStoreSecretName" -}}
{{- default (printf "%s-object-store" (include "styio-platform.fullname" .)) .Values.objectStore.existingSecret -}}
{{- end -}}

{{- define "styio-platform.validateObjectStore" -}}
{{- if eq .Values.objectStore.provider "s3" -}}
{{- if or (not .Values.objectStore.bucket) (not .Values.objectStore.endpoint) -}}
{{- fail "objectStore.provider=s3 requires non-empty objectStore.bucket and objectStore.endpoint" -}}
{{- end -}}
{{- end -}}
{{- end -}}

{{- define "styio-platform.objectStoreEnv" -}}
- name: STYIO_PLATFORM_OBJECT_STORE_PROVIDER
  value: {{ .Values.objectStore.provider | quote }}
- name: STYIO_PLATFORM_OBJECT_STORE_BUCKET
  value: {{ .Values.objectStore.bucket | quote }}
- name: STYIO_PLATFORM_OBJECT_STORE_ENDPOINT
  value: {{ .Values.objectStore.endpoint | quote }}
- name: STYIO_PLATFORM_OBJECT_STORE_REGION
  value: {{ default .Values.region .Values.objectStore.region | quote }}
- name: STYIO_PLATFORM_OBJECT_STORE_PREFIX
  value: {{ .Values.objectStore.prefix | quote }}
- name: STYIO_PLATFORM_OBJECT_STORE_PATH_STYLE
  value: {{ .Values.objectStore.pathStyle | quote }}
- name: STYIO_PLATFORM_OBJECT_STORE_ACCESS_KEY_ID
  valueFrom:
    secretKeyRef:
      name: {{ include "styio-platform.objectStoreSecretName" . }}
      key: access-key-id
- name: STYIO_PLATFORM_OBJECT_STORE_SECRET_ACCESS_KEY
  valueFrom:
    secretKeyRef:
      name: {{ include "styio-platform.objectStoreSecretName" . }}
      key: secret-access-key
- name: STYIO_PLATFORM_OBJECT_STORE_SESSION_TOKEN
  valueFrom:
    secretKeyRef:
      name: {{ include "styio-platform.objectStoreSecretName" . }}
      key: session-token
      optional: true
{{- end -}}

{{- define "styio-platform.mtlsEnv" -}}
- name: STYIO_PLATFORM_TLS_ENABLED
  value: {{ .Values.mtls.tlsEnabled | quote }}
- name: STYIO_PLATFORM_MTLS_REQUIRED
  value: {{ .Values.mtls.required | quote }}
- name: STYIO_PLATFORM_TRUST_PROXY_IDENTITY_HEADERS
  value: {{ .Values.mtls.trustProxyIdentityHeaders | quote }}
{{- if .Values.mtls.caSecretName }}
- name: STYIO_PLATFORM_MTLS_CA
  value: {{ .Values.mtls.caPath | quote }}
{{- end }}
{{- if .Values.mtls.serverSecretName }}
- name: STYIO_PLATFORM_MTLS_CERT
  value: {{ .Values.mtls.certPath | quote }}
- name: STYIO_PLATFORM_MTLS_KEY
  value: {{ .Values.mtls.keyPath | quote }}
{{- end }}
{{- end -}}

{{- define "styio-platform.clientMtlsEnv" -}}
- name: STYIO_PLATFORM_TLS_ENABLED
  value: {{ .Values.mtls.tlsEnabled | quote }}
- name: STYIO_PLATFORM_MTLS_REQUIRED
  value: {{ .Values.mtls.required | quote }}
- name: STYIO_PLATFORM_TRUST_PROXY_IDENTITY_HEADERS
  value: {{ .Values.mtls.trustProxyIdentityHeaders | quote }}
{{- if .Values.mtls.caSecretName }}
- name: STYIO_PLATFORM_MTLS_CA
  value: {{ .Values.mtls.caPath | quote }}
{{- end }}
{{- if .Values.mtls.clientSecretName }}
- name: STYIO_PLATFORM_MTLS_CERT
  value: {{ .Values.mtls.clientCertPath | quote }}
- name: STYIO_PLATFORM_MTLS_KEY
  value: {{ .Values.mtls.clientKeyPath | quote }}
{{- end }}
{{- end -}}
