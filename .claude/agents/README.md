# Agents winx86

Là où un [skill](../skills/README.md) décrit un **domaine** (comment raisonner
sur le dynarec, comment mesurer honnêtement), un agent décrit une **procédure
complète** : une tâche qu'on refait plusieurs fois, dont chaque étape a déjà
coûté une erreur au moins une fois.

| Agent | Quand |
|---|---|
| [`shim-split`](shim-split.md) | Déplacer des shims entre le moteur et un portage — auditer un groupe de DLL, extraire ce qui est réellement générique, laisser le reste avec une raison écrite. Contient le filet `shim_seq` et la règle « une seule inscription par clé » |
| [`doc-update`](doc-update.md) | Remettre `docs/` en accord avec le code après un chantier qui a déplacé la frontière moteur/portage — en vérifiant les faits dans le code, pas dans un résumé |

## Pourquoi ces deux-là et pas d'autres

Ce sont les deux procédures qui, sur ce dépôt, ont produit de vraies régressions
quand elles ont été improvisées : une inscription de shim en double a cassé une
connexion réseau, et une doc restée en arrière a affirmé pendant des semaines le
contraire de ce que le code faisait.

Une procédure déjà correctement décrite dans `docs/` (re-synchroniser le dynarec
en amont, par exemple) n'a pas besoin d'un agent qui la répète : un agent
décoratif dilue ceux qui portent une vraie leçon.
