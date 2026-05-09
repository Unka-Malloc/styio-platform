"""Compatibility facade for package registry v2 key generation."""

import sys

from PublicationBuilder.package_registry_v2 import keygen as _impl

sys.modules[__name__] = _impl
