"""Compatibility facade for package registry v2 static read validation."""

import sys

from StaticReadPlane.package_registry_v2 import validator as _impl

sys.modules[__name__] = _impl
