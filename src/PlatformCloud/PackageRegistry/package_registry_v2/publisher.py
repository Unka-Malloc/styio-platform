"""Compatibility facade for package registry v2 publication building."""

import sys

from PublicationBuilder.package_registry_v2 import publisher as _impl

sys.modules[__name__] = _impl
