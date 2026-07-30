"""Static read-plane validators for package registry v2."""

from .validator import RootReader, verify_registry_root

__all__ = ["RootReader", "verify_registry_root"]
